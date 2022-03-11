#include <Storages/DeltaMerge/ColumnFile/ColumnFile.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileBig.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileDeleteRange.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileTiny.h>

namespace DB
{
namespace DM
{
void serializeSavedColumnFilesInV3Format(WriteBuffer & buf, const ColumnFilePersisteds & column_files)
{
    writeIntBinary(column_files.size(), buf);
    BlockPtr last_schema;

    for (const auto & column_file : column_files)
    {
        // Do not encode the schema if it is the same as previous one.
        writeIntBinary(column_file->getType(), buf);

        switch (column_file->getType())
        {
        case ColumnFile::Type::DELETE_RANGE:
        {
            column_file->serializeMetadata(buf, false);
            break;
        }
        case ColumnFile::Type::BIG_FILE:
        {
            column_file->serializeMetadata(buf, false);
            break;
        }
        case ColumnFile::Type::TINY_FILE:
        {
            auto * tiny_file = column_file->tryToTinyFile();
            auto cur_schema = tiny_file->getSchema();
            if (unlikely(!cur_schema))
                throw Exception("A tiny file without schema: " + column_file->toString(), ErrorCodes::LOGICAL_ERROR);

            bool save_schema = cur_schema != last_schema;
            column_file->serializeMetadata(buf, save_schema);
            break;
        }
        default:
            throw Exception("Unexpected type", ErrorCodes::LOGICAL_ERROR);
        }
    }
}

ColumnFilePersisteds deserializeSavedColumnFilesInV3Format(DMContext & context, const RowKeyRange & segment_range, ReadBuffer & buf, UInt64 /*version*/)
{
    size_t column_file_count;
    readIntBinary(column_file_count, buf);
    ColumnFilePersisteds column_files;
    BlockPtr last_schema;
    for (size_t i = 0; i < column_file_count; ++i)
    {
        std::underlying_type<ColumnFile::Type>::type column_file_type;
        readIntBinary(column_file_type, buf);
        ColumnFilePersistedPtr column_file;
        switch (column_file_type)
        {
        case ColumnFile::Type::DELETE_RANGE:
            column_file = ColumnFileDeleteRange::deserializeMetadata(buf);
            break;
        case ColumnFile::Type::TINY_FILE:
        {
            std::tie(column_file, last_schema) = ColumnFileTiny::deserializeMetadata(buf, last_schema);
            break;
        }
        case ColumnFile::Type::BIG_FILE:
        {
            column_file = ColumnFileBig::deserializeMetadata(context, segment_range, buf);
            break;
        }
        default:
            throw Exception("Unexpected column file type: " + DB::toString(column_file_type), ErrorCodes::LOGICAL_ERROR);
        }
        column_files.emplace_back(std::move(column_file));
    }
    return column_files;
}

} // namespace DM
} // namespace DB