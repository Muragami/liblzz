liblzz - lzz archive/data-stream Library
================================

lzz is a format for achives or data-streams designed to be:
- compact
- efficient
- flexible

It supports compressed solid archives with LZ4 compression 
and uncompressed archives/streams. Files are nominally marked
with the extensions: .lzz and .uzz for lz4 and uncompressed
formats. LZ4 is fast mode is very fast though, so usage of
the uncompressed form is really there more for analysis than
regular usage.

#### 4-byte Chunked Streams

The archive format is detailed in-depth in the lzz.h header
file but here is a summary:

An archive is made of atomic 4-byte/32-bit chunks. Entries
are denoted by a marker chunk and end when a new marker
chunk or a stop chunk is found. Entries are numbered and
must appear in order. You always send marker entry 2 after 
1 and so on. Entry 0 is considered the 'header' of the
stream or archive. It is the only marker entry that never
has or needs and extension info chunk block.

It is possible to send 'nodata' extension chunks which
contain no data chunk blocks at all. In this way you can
store just info and tags alone. This makes sense for a
variety of uses, like sending update data through a
stream which does not hold a data block at all.

Tags are powerful, and chunks can inherit tags from a parent.
You can chain this inheritance to any depth and the parser
respects it.

#### Archive vs Stream Use

The terms here are pretty interchangeable. A stream
is the case of using lzz to communicate (say via a socket)
and is just one Archive after another, each a complete 
message.

#### Arbitrary Limitations

It is a requirement of the format to send the total count
of entries and the total size of the stream in marker
entry zero. This makes it a requirement to know this at the
start of writing a stream. It also makes it easier on
the reader, since it knows at the start how much data
and how many entries to expect.

Entries and total stream size is capped at 48-bit space.
That is 256 terabytes of stream space!

Tags are limited to 256 UTF-8 byte strings for both name
and value. However because you have access to a chunk byte
you can 'chain' 256 values for one name.

#### License

All source material within this repo is MIT licensed.
See [LICENSE](LICENSE) for details.
