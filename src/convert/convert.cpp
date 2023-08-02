#include "convert.hpp"
#include "base/base.hpp"

#include <boost/beast/core/detail/base64.hpp>

namespace bgcode { 
using namespace core;
using namespace base;
namespace convert {

static std::string_view trim(const std::string_view& str)
{
    if (str.empty())
        return std::string_view();
    size_t start = 0;
    while (start < str.size() - 1 && (str[start] == ' ' || str[start] == '\t')) { ++start; }
    size_t end = str.size() - 1;
    while (end > 0 && (str[end] == ' ' || str[end] == '\t')) { --end; }
    if ((start == end && (str[end] == ' ' || str[end] == '\t')) || (start > end))
        return std::string_view();
    else
        return std::string_view(&str[start], end - start + 1);
}

static std::string_view uncomment(const std::string_view& str)
{
    return (!str.empty() && str[0] == ';') ? trim(str.substr(1)) : str;
}

BGCODE_CONVERT_EXPORT EResult from_ascii_to_binary(FILE& src_file, FILE& dst_file)
{
    return EResult::Success;
}

BGCODE_CONVERT_EXPORT EResult from_binary_to_ascii(FILE& src_file, FILE& dst_file, bool verify_checksum)
{
    auto write_line = [&](const std::string& line) {
        fwrite(line.data(), 1, line.length(), &dst_file);
        return !ferror(&dst_file);
    };

    auto write_metadata = [&](const std::vector<std::pair<std::string, std::string>>& data) {
        for (const auto& [key, value] : data) {
            if (!write_line("; " + key + " = " + value + "\n"))
                return false;
        }
        return !ferror(&dst_file);
    };

    EResult res = is_valid_binary_gcode(src_file, true);
    if (res != EResult::Success)
        // propagate error
        return res;

    fseek(&src_file, 0, SEEK_END);
    const long file_size = ftell(&src_file);
    rewind(&src_file);

    //
    // read file header
    //
    FileHeader file_header;
    res = read_header(src_file, file_header, nullptr);
    if (res != EResult::Success)
        // propagate error
        return res;

    //
    // convert file metadata block
    //
    BlockHeader block_header;
    res = read_next_block_header(src_file, file_header, block_header, verify_checksum);
    if (res != EResult::Success)
        // propagate error
        return res;
    if ((EBlockType)block_header.type != EBlockType::FileMetadata)
        return EResult::InvalidSequenceOfBlocks;
    FileMetadataBlock file_metadata_block;
    res = file_metadata_block.read_data(src_file, file_header, block_header);
    if (res != EResult::Success)
        // propagate error
        return res;
    auto producer_it = std::find_if(file_metadata_block.raw_data.begin(), file_metadata_block.raw_data.end(),
        [](const std::pair<std::string, std::string>& item) { return item.first == "Producer"; });
    const std::string producer_str = (producer_it != file_metadata_block.raw_data.end()) ? producer_it->second : "Unknown";
    if (!write_line("; generated by " + producer_str + "\n\n\n"))
        return EResult::WriteError;

    //
    // convert printer metadata block
    //
    res = read_next_block_header(src_file, file_header, block_header, verify_checksum);
    if (res != EResult::Success)
        // propagate error
        return res;
    if ((EBlockType)block_header.type != EBlockType::PrinterMetadata)
        return EResult::InvalidSequenceOfBlocks;
    PrinterMetadataBlock printer_metadata_block;
    res = printer_metadata_block.read_data(src_file, file_header, block_header);
    if (res != EResult::Success)
        // propagate error
        return res;
    if (!write_metadata(printer_metadata_block.raw_data))
        return EResult::WriteError;

    //
    // convert thumbnail blocks
    //
    long restore_position = ftell(&src_file);
    res = read_next_block_header(src_file, file_header, block_header, verify_checksum);
    if (res != EResult::Success)
        // propagate error
        return res;
    while ((EBlockType)block_header.type == EBlockType::Thumbnail) {
        ThumbnailBlock thumbnail_block;
        res = thumbnail_block.read_data(src_file, file_header, block_header);
        if (res != EResult::Success)
            // propagate error
            return res;
        static constexpr const size_t max_row_length = 78;
        std::string encoded;
        encoded.resize(boost::beast::detail::base64::encoded_size(thumbnail_block.data.size()));
        encoded.resize(boost::beast::detail::base64::encode((void*)encoded.data(), (const void*)thumbnail_block.data.data(), thumbnail_block.data.size()));
        std::string format;
        switch ((EThumbnailFormat)thumbnail_block.format)
        {
        default:
        case EThumbnailFormat::PNG: { format = "thumbnail"; break; }
        case EThumbnailFormat::JPG: { format = "thumbnail_JPG"; break; }
        case EThumbnailFormat::QOI: { format = "thumbnail_QOI"; break; }
        }
        if (!write_line("\n;\n; " + format + " begin " + std::to_string(thumbnail_block.width) + "x" + std::to_string(thumbnail_block.height) +
            " " + std::to_string(encoded.length()) + "\n"))
            return EResult::WriteError;
        while (encoded.size() > max_row_length) {
            if (!write_line("; " + encoded.substr(0, max_row_length) + "\n"))
                return EResult::WriteError;
            encoded = encoded.substr(max_row_length);
        }
        if (encoded.size() > 0) {
            if (!write_line("; " + encoded + "\n"))
                return EResult::WriteError;
        }
        if (!write_line("; " + format + " end\n;\n"))
            return EResult::WriteError;

        restore_position = ftell(&src_file);
        res = read_next_block_header(src_file, file_header, block_header, verify_checksum);
        if (res != EResult::Success)
            // propagate error
            return res;
    }

    //
    // convert gcode blocks
    //
    auto remove_empty_lines = [](const std::string& data) {
        std::string ret;
        auto begin_it = data.begin();
        auto end_it = data.begin();
        while (end_it != data.end()) {
            while (end_it != data.end() && *end_it != '\n') {
                ++end_it;
            }

          const size_t pos = std::distance(data.begin(), begin_it);
          const size_t line_length = std::distance(begin_it, end_it);
          const std::string_view original_line(&data[pos], line_length);
          const std::string_view reduced_line = uncomment(trim(original_line));
          if (!reduced_line.empty())
              ret += std::string(original_line) + "\n";
          begin_it = ++end_it;
        }

        return ret;
    };

    if (!write_line("\n"))
        return EResult::WriteError;
    res = skip_block_content(src_file, file_header, block_header);
    if (res != EResult::Success)
        // propagate error
        return res;
    res = read_next_block_header(src_file, file_header, block_header, EBlockType::GCode, verify_checksum);
    if (res != EResult::Success)
        // propagate error
        return res;
    while ((EBlockType)block_header.type == EBlockType::GCode) {
        GCodeBlock block;
        res = block.read_data(src_file, file_header, block_header);
        if (res != EResult::Success)
            // propagate error
            return res;
        const std::string out_str = remove_empty_lines(block.raw_data);
        if (!out_str.empty()) {
            if (!write_line(out_str))
                return EResult::WriteError;
        }
        if (ftell(&src_file) == file_size)
            break;
        res = read_next_block_header(src_file, file_header, block_header, verify_checksum);
        if (res != EResult::Success)
            // propagate error
            return res;
    }

    //
    // convert print metadata block
    //
    fseek(&src_file, restore_position, SEEK_SET);
    res = read_next_block_header(src_file, file_header, block_header, verify_checksum);
    if (res != EResult::Success)
        // propagate error
        return res;
    if ((EBlockType)block_header.type != EBlockType::PrintMetadata)
        return EResult::InvalidSequenceOfBlocks;
    PrintMetadataBlock print_metadata_block;
    res = print_metadata_block.read_data(src_file, file_header, block_header);
    if (res != EResult::Success)
        // propagate error
        return res;
    if (!write_line("\n"))
        return EResult::WriteError;
    if (!write_metadata(print_metadata_block.raw_data))
        return EResult::WriteError;

    //
    // convert slicer metadata block
    //
    res = read_next_block_header(src_file, file_header, block_header, verify_checksum);
    if (res != EResult::Success)
        // propagate error
        return res;
    if ((EBlockType)block_header.type != EBlockType::SlicerMetadata)
        return EResult::InvalidSequenceOfBlocks;
    SlicerMetadataBlock slicer_metadata_block;
    res = slicer_metadata_block.read_data(src_file, file_header, block_header);
    if (res != EResult::Success)
        // propagate error
        return res;
    if (!write_line("\n; prusaslicer_config = begin\n"))
        return EResult::WriteError;
    if (!write_metadata(slicer_metadata_block.raw_data))
        return EResult::WriteError;
    if (!write_line("; prusaslicer_config = end\n\n"))
        return EResult::WriteError;

    return EResult::Success;
}

} // namespace core
} // namespace bgcode
