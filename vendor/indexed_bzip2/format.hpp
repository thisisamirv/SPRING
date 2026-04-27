#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include <FileReader.hpp>
#include <Shared.hpp>
#include <Bgzf.hpp>
#include <deflate.hpp>
#include <gzip.hpp>


namespace rapidgzip
{
[[nodiscard]] inline std::optional<std::pair<FileType, /* offset */ size_t> >
determineFileTypeAndOffset( const UniqueFileReader& fileReader )
{
    if ( !fileReader ) {
        return std::nullopt;
    }

    /* The first deflate block offset is easily found by reading over the gzip header.
     * The correctness and existence of this first block is a required initial condition for the algorithm. */
    gzip::BitReader bitReader{ fileReader->clone() };
    const auto [gzipHeader, gzipError] = gzip::readHeader( bitReader );
    if ( gzipError == Error::NONE ) {
        return std::make_pair( blockfinder::Bgzf::isBgzfFile( fileReader ) ? FileType::BGZF : FileType::GZIP,
                               bitReader.tell() );
    }

    /** Try reading zlib header */
    bitReader.seek( 0 );
    const auto [zlibHeader, zlibError] = zlib::readHeader( bitReader );
    if ( zlibError == Error::NONE ) {
        return std::make_pair( FileType::ZLIB, bitReader.tell() );
    }

    /* Try deflate last because it has the least redundancy. In the worst case, for fixed Huffman blocks,
     * it checks only 1 bit! */
    bitReader.seek( 0 );
    deflate::Block block;
    if ( block.readHeader( bitReader ) == Error::NONE ) {
        return std::make_pair( FileType::DEFLATE, 0 );
    }

    return std::nullopt;
}


#ifdef WITH_PYTHON_SUPPORT
[[nodiscard]] std::string
determineFileTypeAsString( PyObject* pythonObject )
{
    const auto detectedType = determineFileTypeAndOffset(
        ensureSharedFileReader( std::make_unique<PythonFileReader>( pythonObject ) ) );
    return toString( detectedType ? detectedType->first : FileType::NONE );
}
#endif
}  // namespace rapidgzip
