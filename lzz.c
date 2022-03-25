/*
	see lzz.h for details
*/

#include "lzz.h"
#include "include/lz4io.h"
#include "include/lz4hc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define IO_INTERNAL(x)		(((lzzIO*)x)->internal)
#define IO_CONTEXT(x)		(((lzzIO*)x)->context)
#define IO_ERR(x)			(((lzzIO*)x)->err)
#define IO(x)				((lzzIO*)x)
#define ERR_ON_NULL(x,c,m)  if (x == NULL) c->errorHandler(m);

unsigned int _lzzElfHash(unsigned int *hash, const unsigned char *str, unsigned int length)
{
	unsigned int hval = *hash;
	unsigned int x = 0;
	unsigned int i = 0;

	for (i = 0; i < length; ++str, ++i) {
    	hval = (hval << 4) + (*str);
		if ((x = hval & 0xF0000000L) != 0) hval ^= (x >> 24);
		hval &= ~x;
	}

	return hval;
}

// default error is dump to stderror and exit the app
void _lzzStdError(const char *error)
{
	fprintf(stderr, error);
	exit(-1);
}

void *_lzzMalloc(lzzContext *c, unsigned int bytes, const char* emsg)
{
	void *ret = c->malloc(bytes);
	ERR_ON_NULL(ret,c,emsg)
	c->bytesAllocated += bytes;
	return ret;
}

void *_lzzCalloc(lzzContext *c, unsigned int bytes, const char* emsg)
{
	void *ret = c->calloc(1, bytes);
	ERR_ON_NULL(ret,c,emsg)
	c->bytesAllocated += bytes;
	return ret;
}

void *_lzzRealloc(lzzContext *c, void *p, unsigned int bytes, unsigned int old_bytes, const char* emsg)
{
	void *ret = c->realloc(p, bytes);
	ERR_ON_NULL(ret,c,emsg)
	c->bytesAllocated += bytes - old_bytes;
	return ret;
}

void _lzzFree(lzzContext *c, void *p, unsigned int bytes)
{
	c->free(p);
	c->bytesAllocated -= bytes;
}

#define LZZ_BLOCK_SIZE		8192		// 8k blocks

#define LZZC_STOP 			{ .byte={ 4, 0, 0, 0 } }
#define LZZC_MARKER0		{ .byte={ 0, 0, 0, 0 } }

#define LZZ_CHUNK_MARKER	0
#define LZZ_CHUNK_TAG		1
#define LZZ_CHUNK_INFO		2
#define LZZ_CHUNK_DATA		3
#define LZZ_CHUNK_STOP		4

#define LZZ_MEM_REMOTE_BUFFER	1

#define LZZ_LZ4_WRITE 			1
#define LZZ_LZ4_FILE 			2

int _lzzFileRead(void *io, char *buffer, int bufferLength)
{
	FILE *f = (FILE*)IO_INTERNAL(io);
	size_t ret;

	ret = fread(buffer, 1, (size_t)bufferLength, f);

	if (ret != bufferLength) {
		int e = ferror(f);
		if (e) {
			strncpy(IO_ERR(io),strerror(e),256);
			IO_ERR(io)[255] = 0; 
		}
	}

	if (ret == 0) return -1;
	 else return (int)ret;
}

int _lzzFileWrite(void *io, char *data, int dataLength)
{
	FILE *f = (FILE*)IO_INTERNAL(io);
	size_t ret;

	ret = fwrite(data, 1, (size_t)dataLength, f);
	int e = ferror(f);
	if (e) {
		strncpy(IO_ERR(io),strerror(e),256);
		IO_ERR(io)[255] = 0; 
	}

	if (ret != (size_t)dataLength) return -1;
	 else return (int)ret;
}

void _lzzFileDone(void *io)
{
	fclose((FILE*)IO_INTERNAL(io));
}

lzzIO lzzCreateFileIO(lzzContext *c, const char *fname, const char *mode)
{
	lzzIO io;

	memset(io.err,0,256);
	io.context = c;
	io.internal = (void*)fopen(fname, mode);
	io.read = _lzzFileRead;
	io.write = _lzzFileWrite;
	io.done = _lzzFileDone;

	int e = ferror(io.internal);
	if (e) {
		strncpy(io.err,strerror(e),256);
		io.err[255] = 0; 
	}

	return io;
}

typedef struct _lzzMemRefInteral {
	const char *bytes;
	int flags;
	unsigned int maxBytes;
	unsigned int pos;
	unsigned int maxDouble;
} lzzMemRefInteral;

typedef struct _lzzMemInteral {
	char *bytes;
	int flags;
	unsigned int maxBytes;
	unsigned int pos;
	unsigned int maxDouble;
} lzzMemInteral;

int _lzzMemRead(void *io, char *buffer, int bufferLength)
{
	lzzMemInteral *mem = (lzzMemInteral*)IO_INTERNAL(io);
	if (mem->maxBytes - mem->pos) return -1;	// EOF on mem
	if (bufferLength > (mem->maxBytes - mem->pos)) {
		bufferLength = mem->maxBytes - mem->pos;
	}
	memcpy(buffer, mem->bytes + mem->pos, bufferLength);
	mem->pos += bufferLength;
	return bufferLength;
}

int _lzzMemWrite(void *io, char *data, int dataLength)
{
	lzzMemInteral *mem = (lzzMemInteral*)IO_INTERNAL(io);
	if (dataLength > (mem->maxBytes - mem->pos)) {
		if (mem->flags & LZZ_MEM_REMOTE_BUFFER) {
			// this is an error, we can't expand buffers we don't own!
			strcpy(IO_ERR(io),"lzzIO can't expand a remote buffer (write failure).");
			return -2;
		}
		unsigned int nlen = mem->maxBytes;
		if (nlen < mem->maxDouble) {
			nlen = nlen << 1;
		} else {
			nlen += mem->maxDouble;
		}
		mem->bytes = _lzzRealloc(IO_CONTEXT(io), mem->bytes, nlen, mem->maxBytes, "lzzIO failed on realloc (write failure).");
		mem->maxBytes = nlen;
	}
	memcpy(mem->bytes + mem->pos, data, dataLength);
	mem->pos += dataLength;
	return dataLength;
}

void _lzzMemDone(void *p) { }

lzzIO lzzCreateMemIONew(lzzContext *c, unsigned int initialBytes, unsigned int maxDouble)
{
	lzzIO io;

	lzzMemInteral *mem = _lzzCalloc(c, sizeof(lzzMemInteral), "lzzCreateMemIONew() struct allocation failed.");
	
	mem->maxDouble = maxDouble;
	mem->maxBytes = initialBytes;
	mem->bytes = _lzzMalloc(c, initialBytes, "lzzCreateMemIONew() byte allocation failed."); 
	mem->pos = 0;
	mem->flags = 0;

	memset(io.err,0,256);
	io.context = c;
	io.internal = mem;
	io.read = _lzzMemRead;
	io.write = _lzzMemWrite;
	io.done = _lzzMemDone;

	return io;
}

lzzIO lzzCreateMemIOBuffer(lzzContext *c, const void *buffer, unsigned int bufferBytes, unsigned int maxDouble)
{
	lzzIO io;

	lzzMemRefInteral *mem = _lzzCalloc(c,sizeof(lzzMemRefInteral),"lzzCreateMemIOBuffer() struct allocation failed.");

	mem->maxDouble = maxDouble;
	mem->maxBytes = bufferBytes;
	mem->bytes = buffer;
	mem->pos = 0;
	mem->flags = LZZ_MEM_REMOTE_BUFFER;	// make sure we don't free this on release

	memset(io.err,0,256);
	io.context = c;
	io.internal = mem;
	io.read = _lzzMemRead;
	io.write = _lzzMemWrite;
	io.done = _lzzMemDone;

	return io;
}

lzzIO lzzCreateMemIOBufferCopy(lzzContext *c, void *buffer, unsigned int bufferBytes, unsigned int maxDouble)
{
	lzzIO io;

	io = lzzCreateMemIONew(c, bufferBytes, (1 << 23));
	lzzMemInteral *mem = (lzzMemInteral *)io.internal;
	memcpy(mem->bytes, buffer, mem->maxBytes);

	return io;
}

void lzzDestroyMemIO(lzzContext *c, lzzIO *io)
{
	lzzMemInteral *mem = (lzzMemInteral *)io->internal;
	if (!(mem->flags & LZZ_MEM_REMOTE_BUFFER)) {
		// free the bytes
		_lzzFree(c, mem->bytes, mem->maxBytes);
	}
	_lzzFree(c, mem, sizeof(lzzMemInteral));
}

typedef struct _lzzLz4Interal {
	XIO xio;
	union {
		LZ4_writeIO_t* lz4fWrite;
		LZ4_readIO_t* lz4fRead;
	};
    int flags;
} lzzLz4Interal;

size_t xioFileRead(void *ptr, size_t size, size_t count, void* io)
{
	return fread(ptr, size, count, ((XIO*)io)->internal);
}

size_t xioFileWrite(void * ptr, size_t size, size_t count, void* io)
{
	return fwrite(ptr, size, count, ((XIO*)io)->internal);
}

XIO _lzzFileXIO(lzzContext *c, const char *fname, const char *mode)
{
	XIO x;
	x.internal = fopen(fname, mode);
	x.read = xioFileRead;
	x.write = xioFileWrite;
	return x;
}

typedef struct _lzzXioMemRefInteral {
	XIO xio;
	const char *bytes;
	unsigned int pos;
	unsigned int length;
    int flags;
} lzzXioMemRefInteral;

size_t xioMemRefRead(void *ptr, size_t size, size_t count, void* io)
{
	size_t bytes = size * count;
	lzzXioMemRefInteral *mem = (lzzXioMemRefInteral*)(((XIO*)io)->internal);
	if (mem->pos + bytes > mem->length) {
		bytes = mem->length - mem->pos;
	}
	if (bytes > 0) {
		memcpy(ptr, mem->bytes + mem->pos, bytes);
		mem->pos += bytes;
	}
	return bytes;
}

size_t xioMemRefWrite(void * ptr, size_t size, size_t count, void* io)
{
	return -2; // hell no, how?
}

XIO _lzzMemReadXIO(lzzContext *c, const char *buffer, unsigned int len)
{
	XIO x;
	lzzXioMemRefInteral *m = _lzzCalloc(c, sizeof(lzzXioMemRefInteral), "_lzzMemReadXIO() allocation failed.");
	
	m->bytes = buffer;
	m->length = len;
	m->flags = LZZ_MEM_REMOTE_BUFFER;

	x.internal = m;
	x.read = xioMemRefRead;
	x.write = xioMemRefWrite;

	return x;
}

typedef struct _lzzXioMemInteral {
	XIO xio;
	char *bytes;
	unsigned int pos;
	unsigned int length;
	unsigned int maxDouble;
	lzzContext *c;
    int flags;
} lzzXioMemInteral;

size_t xioMemRead(void *ptr, size_t size, size_t count, void* io)
{
	size_t bytes = size * count;
	lzzXioMemInteral *mem = (lzzXioMemInteral*)(((XIO*)io)->internal);
	if (mem->pos + bytes > mem->length) {
		bytes = mem->length - mem->pos;
	}
	if (bytes > 0) {
		memcpy(ptr, mem->bytes + mem->pos, bytes);
		mem->pos += bytes;
	}
	return bytes;
}

size_t xioMemWrite(void * ptr, size_t size, size_t count, void* io)
{
	size_t bytes = size * count;
	lzzXioMemInteral *mem = (lzzXioMemInteral*)(((XIO*)io)->internal);
	if (mem->pos + bytes > mem->length) {
		unsigned int nlen = mem->length;
		if (mem->pos + bytes < mem->maxDouble) {
			while (nlen < (mem->pos + bytes))
				nlen = nlen << 1;
		} else {
			while (nlen < (mem->pos + bytes))
				nlen += mem->maxDouble;
		}
		mem->bytes = _lzzRealloc(mem->c, mem->bytes, nlen, mem->length, "xioMemWrite() realloc failed.");
		mem->length = nlen;
	}
	memcpy(mem->bytes + mem->pos, ptr, bytes);
	mem->pos += bytes;
	return bytes;
}

XIO _lzzMemWriteXIO(lzzContext *c, unsigned int len, unsigned int maxDouble)
{
	XIO x;
	lzzXioMemInteral *m = _lzzCalloc(c, sizeof(lzzXioMemRefInteral), "_lzzMemWriteXIO() allocation failed.");
	
	m->bytes = _lzzMalloc(c, len, "_lzzMemWriteXIO() dynamic allocation failed.");
	m->length = len;
	m->maxDouble = maxDouble;
	m->flags = LZZ_MEM_REMOTE_BUFFER;
	m->c = c;

	x.internal = m;
	x.read = xioMemRead;
	x.write = xioMemWrite;

	return x;
}

int _lzzLz4Read(void *io, char *buffer, int bufferLength)
{
	lzzLz4Interal *i = IO_INTERNAL(io);
	if (i->flags & LZZ_LZ4_WRITE) return -2;				// calling read from a write IO???
	
	size_t read = LZ4FIO_read(i->lz4fRead, buffer, bufferLength);
	if (read == 0) return -1;

	return read;
}

int _lzzLz4Write(void *io, char *data, int dataLength)
{
	lzzLz4Interal *i = IO_INTERNAL(io);
	if (!(i->flags & LZZ_LZ4_WRITE)) return -2;				// calling write from a read IO???
	
	size_t wrote = LZ4FIO_write(i->lz4fWrite, data, dataLength);

	return wrote;
}

void _lzzLz4Done(void *io)
{
	lzzLz4Interal *i = IO_INTERNAL(io);
	LZ4F_errorCode_t r;
	if (i->flags & LZZ_LZ4_WRITE) {
		r = LZ4FIO_writeClose(i->lz4fWrite);
		if (LZ4F_isError(r)) {
			strncpy(IO(io)->err,LZ4F_getErrorName(r),256);
			IO(io)->err[255] = 0;
		}
	} else {
		r = LZ4FIO_readClose(i->lz4fRead);
		if (LZ4F_isError(r)) {
			strncpy(IO(io)->err,LZ4F_getErrorName(r),256);
			IO(io)->err[255] = 0;
		}
	}
	if (i->flags & LZZ_LZ4_FILE) {
		// close the file
		fclose(i->xio.internal);
	}
}

lzzIO lzzCreateLz4FastFileIOWrite(lzzContext *c, const char *fname)
{
	lzzIO io;
	lzzLz4Interal *i;
	LZ4F_errorCode_t r;

	i = _lzzCalloc(c, sizeof(lzzLz4Interal), "lzzCreateLz4FastFileIOWrite() internal allocation failed.");
	i->xio = _lzzFileXIO(c, fname, "w");
	r = LZ4FIO_writeOpen(&i->lz4fWrite, &i->xio, NULL);
	if (LZ4F_isError(r)) {
		strncpy(io.err,LZ4F_getErrorName(r),256);
		io.err[255] = 0;
	}
	i->flags = LZZ_LZ4_WRITE | LZZ_LZ4_FILE;
	io.internal = i;
	io.read = _lzzLz4Read;
	io.write = _lzzLz4Write;
	io.done = _lzzLz4Done;

	return io;
}

lzzIO lzzCreateLz4FileIORead(lzzContext *c, const char *fname)
{
	lzzIO io;
	lzzLz4Interal *i;
	LZ4F_errorCode_t r;

	i = _lzzCalloc(c, sizeof(lzzLz4Interal), "lzzCreateLz4FastFileIORead() internal allocation failed.");
	i->xio = _lzzFileXIO(c, fname, "r");
	r = LZ4FIO_readOpen(&i->lz4fRead, &i->xio);
	if (LZ4F_isError(r)) {
		strncpy(io.err,LZ4F_getErrorName(r),256);
		io.err[255] = 0;
	}
	i->flags = LZZ_LZ4_FILE;
	io.read = _lzzLz4Read;
	io.write = _lzzLz4Write;
	io.done = _lzzLz4Done;

	return io;
}

lzzIO lzzCreateLz4MemIORead(lzzContext *c, const char *buffer, unsigned int len)
{
	lzzIO io;
	lzzLz4Interal *i;
	LZ4F_errorCode_t r;

	i = _lzzCalloc(c, sizeof(lzzLz4Interal), "lzzCreateLz4FastFileIORead() internal allocation failed.");
	i->xio = _lzzMemReadXIO(c, buffer, len);
	r = LZ4FIO_readOpen(&i->lz4fRead, &i->xio);
	if (LZ4F_isError(r)) {
		strncpy(io.err,LZ4F_getErrorName(r),256);
		io.err[255] = 0;
	}
	i->flags = 0;
	io.read = _lzzLz4Read;
	io.write = _lzzLz4Write;
	io.done = _lzzLz4Done;

	return io;
}

// level is: -5 to +3, 0 is HC default
lzzIO lzzCreateLz4HCFileIOWrite(lzzContext *c, const char *fname, int level)
{
	lzzIO io;
	lzzLz4Interal *i;
	LZ4F_errorCode_t r;
	LZ4F_preferences_t hcpref = { LZ4F_INIT_FRAMEINFO, LZ4HC_CLEVEL_DEFAULT + level, 0u, 0u, { 0u, 0u, 0u } };

	i = _lzzCalloc(c, sizeof(lzzLz4Interal), "lzzCreateLz4FastFileIOWrite() internal allocation failed.");
	i->xio = _lzzFileXIO(c, fname, "w");
	r = LZ4FIO_writeOpen(&i->lz4fWrite, &i->xio, &hcpref);
	if (LZ4F_isError(r)) {
		strncpy(io.err,LZ4F_getErrorName(r),256);
		io.err[255] = 0;
	}
	i->flags = LZZ_LZ4_WRITE | LZZ_LZ4_FILE;
	io.read = _lzzLz4Read;
	io.write = _lzzLz4Write;
	io.done = _lzzLz4Done;

	return io;
}

int _lzzAddArchiveError(lzzContext *c, lzzArchive *a, const char *err)
{
	int cnt = 0;
	lzzError *last = NULL, *n, *p = a->err;
	while (p != NULL && cnt < 512) {
		last = p;
		p = p->next;
		cnt += 1;
	}
	if (cnt == 512) return cnt;
	n = _lzzCalloc(c, sizeof(lzzError),"_lzzAddArchiveError: lzzError allocation failed.");
	if (last == NULL) {
		a->err = n;
	} else {
		last->next = n;
	}
	n->error = strdup(err);
	return cnt;
}

// read the block into a temporary buffer, limited to 8kb blocks
void _lzzScanBlock(lzzParserState *state, lzzIO *io, int len, const char **err, unsigned long long *err_pos) 
{
	if (len > 8192) len = 8192;
	int rd = io->read(io, state->seek, len);
	if (rd != len) {
		// error!
		*err = "data chunk read invalid length";
		*err_pos = state->pos;
	} else {
		state->hash = _lzzElfHash(&state->hash, (const unsigned char*)state->seek, len);
		state->pos += len;
	}
}

void _lzzReadBlock(lzzContext *c, lzzParserState *state, lzzChunk *first, lzzBlock *b, lzzIO *io, int len, const char **err, unsigned long long *err_pos) 
{
	b->chunk = _lzzCalloc(c,sizeof(lzzChunk) + len,"_lzzReadBlock() allocation failed");
	memcpy(b->chunk,&first,4);
	int rd = io->read(io, (char*)&b->chunk[1], len);
	if (rd != len) {
		// error!
		*err = "data chunk read invalid length";
		*err_pos = state->pos;
	} else {
		state->hash = _lzzElfHash(&state->hash, (const unsigned char*)&b->chunk[1], len);
		state->pos += len;
	}
}

// force 4-byte alignment for reading chunks
static inline unsigned int _lzzChunkLen(unsigned int sz)
{
	unsigned int ret = sz >> 2;
	if ((sz % 4) > 0) ret++;
	return ret << 2;
}

void _lzzSkipBlock(lzzParserState *state, lzzIO *io, int len, const char **err, unsigned long long *err_pos) 
{
	while (len > 0) {
		int nibble = len;
		if (nibble > 8192) nibble = 8192;
		int rd = io->read(io, &state->seek[0], nibble);
		if (rd != nibble) {
			// error!
			*err = "data chunk stream read error";
			*err_pos = state->pos;
			return;
		} else {
			state->hash = _lzzElfHash(&state->hash, (const unsigned char*)&state->seek[0], nibble);
			state->pos += nibble;
		}
		len -= nibble;
	}
}

// returns -1 on stream end or error, 0 if a block is skipped, or the total chunks read
unsigned int _lzzIoGetNextBlock(lzzContext *c, lzzArchive *arc, lzzParserState *state, lzzBlock *b, lzzIO *io, int method)
{
	char buffer[128];
	const char *err = NULL;
	unsigned long long err_pos = 0; 
	unsigned int ret = 0;
	int cnt;
	lzzChunk data;

	cnt = io->read(io, (char*)&data, 4);
	if (cnt != 4) return -1;	// stream has ended (hopefully!)
	state->hash = _lzzElfHash(&state->hash, (const unsigned char*)&data, 4);
	state->pos += 4;

	// let's see what is coming
	switch (data.byte[0]) {
		case 0: // marker, nothing!
		case 4: // stop, nothing!
			// these are single chunk by definition so we are already done!
			b->chunk = _lzzCalloc(c,sizeof(lzzChunk),"_lzzIoGetNextBlock() allocation failed");
			memcpy(b->chunk,&data,4);
			b->count = 1;
			return 1;
		break;
		case 1: { // tag, you're it!
			unsigned int sz = (unsigned int)data.byte[1] + (unsigned int)data.byte[2];
			unsigned int len = _lzzChunkLen(sz);
			if ((method & 0xFF) != LZZ_READ_MINIMAL) {
				// read all the tags!
				_lzzReadBlock(c, state, &data, b, io, len, &err, &err_pos);
				ret = len >> 2;
			} else {
				// read only 'title' tags, kind of fake we end up reading in everything anyway no matter what because
				// how else do we know the name of the tag???!
				_lzzScanBlock(state, io, len, &err, &err_pos);
				if (!memcmp(state->seek,"title",5)) {
					b->chunk = _lzzCalloc(c,sizeof(lzzChunk) + len,"_lzzIoGetNextBlock() allocation failed");
					memcpy(b->chunk,state->seek,len);
					ret = len >> 2;
				} else {
					ret = 0;
				}
			}
		}
		break;
		case 2: { // info, get news!
			unsigned int sz;
			if (data.byte[1] > 0x7F) {
 				sz = (unsigned int)data.byte[2] + ((unsigned int)data.byte[3] << 8) * 4;
			} else {
				sz = 4;
			}
			if ((method & 0xFF) != LZZ_READ_MINIMAL) {
				// read all the infos!
				b->chunk = _lzzCalloc(c,sizeof(lzzChunk) + sz,"_lzzIoGetNextBlock() allocation failed");
				// read the data block in
				_lzzReadBlock(c, state, &data, b, io, sz, &err, &err_pos);
				ret = sz >> 2;
			} else {
				// read just the core infos
				switch (data.byte[1]) {
					case 0x00:
					case 0x01:
					case 0x02:
					case 0x03:
					case 0x04:
					case 0x05:
					case 0x06:
					case 0x07:
					case 0x80:
						_lzzReadBlock(c, state, &data, b, io, sz, &err, &err_pos);
						ret = sz >> 2;
					break;

					default:
						_lzzSkipBlock(state, io, sz, &err, &err_pos);
					break;
				}
			}
		}
		break;
		case 3: { // data, the good stuff!
			if (data.byte[1] > 2) {
				// error!
				err = "data chunk type byte invalid";
				err_pos = state->pos;
			} else {
				unsigned int sz = (unsigned int)data.byte[2] + ((unsigned int)data.byte[3] << 8);
				if (data.byte[1] == 2) {
					if (data.byte[2] == 1) {
						sz = 32;
					} else if (data.byte[2] == 1) {
						sz = 64;
					} else {
						// error!
						err = "data hash chunk type byte invalid";
						err_pos = state->pos;
					}
				}
				unsigned int len = _lzzChunkLen(sz);
				if ((method & 0xFF) != LZZ_READ_FULL) {
					// we are skipping this, so let's do that!
					_lzzSkipBlock(state, io, len, &err, &err_pos);
				} else {
					// read the data block in
					_lzzReadBlock(c, state, &data, b, io, len, &err, &err_pos);
					ret = sz >> 2;
				}
			}
		}
		break;

		default: // custom chunk, see if we ignore or callback
			if (c->custom[data.byte[0]]) {
				unsigned int sz = (unsigned int)data.byte[2] + ((unsigned int)data.byte[3] << 8);
				unsigned int len = _lzzChunkLen(sz);
				int rd = c->custom[data.byte[0]](arc, &data, NULL, sz);
				if (rd > 0) {
					if (rd * sizeof(lzzChunk) > len) {
						// error!
						err = "custom chunk read request to long";
						err_pos = state->pos;
					} else {
						_lzzReadBlock(c, state, &data, b, io, len, &err, &err_pos);
						c->custom[data.byte[0]](arc, &data, &b->chunk[1], sz);
					}
				}
			}

		break;
	}

	if (err != NULL) {
		snprintf(buffer,128,"[%llX] %s",err_pos,err);
		buffer[127] =0;
		_lzzAddArchiveError(c, arc, buffer);
	}

	return ret;
}

lzzContext *lzzCreateContext(LZZERROR err, LZZCALLOC c)
{
	lzzContext *ret;

	if (c == NULL) c = calloc;
	if (err == NULL) err = _lzzStdError;
	ret = c(1, sizeof(lzzContext));
	if (ret == NULL) err("lzzCreateContext() allocation failed.");
	ret->errorHandler = err;

	ret->customInfoLimit = 4096;
	lzzMakeMemoryContext(ret, malloc, calloc, realloc, free);

	return ret;
}

void lzzDestroyContext(lzzContext *context)
{
	context->free(context);
}

void lzzMakeSafeContext(lzzContext *context, LZZMUTEX m, LZZLOCK l, LZZUNLOCK u)
{
	context->mutex = m;
	context->lock = l;
	context->unlock = u;
}

void lzzMakeMemoryContext(lzzContext *context, LZZMALLOC m, LZZCALLOC c, LZZREALLOC r, LZZFREE f)
{
	context->malloc = m;
	context->calloc = c;
	context->realloc = r;
	context->free = f;
}

void lzzSetCustomCallback(lzzContext *context, int typeCode, LZZ_CUSTOM_CALLBACK cb)
{
	context->custom[typeCode & 0xFF] = cb;
}


unsigned long long lzzSizeOfMemoryContextFastLimited(lzzContext *context, unsigned long max_entries, unsigned long max_tags,
						unsigned long max_info, unsigned long max_data)
{
	unsigned long long mem = 0;
	mem += max_tags * 516;
	mem += max_info * 2 + context->customInfoLimit + 128;	
			// set aside bytes for custom info blocks based on the context setting, defaults to 4kb and 128 for the MIME info block
	mem += max_data;
	return mem * max_entries;
}

void lzzMakeMemoryContextFastLimited(lzzContext *context, unsigned long max_entries, unsigned long max_tags,
						unsigned long max_info, unsigned long max_data)
{

}

lzzArchive* _lzzReturnErrorArchive(lzzContext *context, const char *error)
{
	lzzArchive *arc;
	arc = _lzzCalloc(context, sizeof(lzzArchive), "_lzzReturnErrorArchive() struct allocation failed.");
	if (context->mutex) {
		arc->safety = context->mutex();
		context->lock(arc->safety);
	}
	_lzzAddArchiveError(context, arc, error);
	if (context->mutex) context->unlock(arc->safety);
	return arc;
}

lzzArchive* lzzScanFile(lzzContext *context, const char *fname, int method)
{
	lzzChunk lz4MagicBytes = { .byte = { 0x04, 0x22, 0x4D, 0x18 } };
	lzzChunk markerBytes = { .byte = { 0x0, 0x0, 0x0, 0x0 } };
	lzzChunk test;
	lzzArchive *arc;
	lzzIO io;
	FILE *fp;

	fp = fopen(fname,"r");
	if (fp == NULL) return _lzzReturnErrorArchive(context, strerror(errno));
	fread(&test, 1, 4, fp);
	fclose(fp);

	if (!memcmp(&test, &lz4MagicBytes, 4)) {
		// it is an lz4 file, handle that
		io = lzzCreateLz4FileIORead(context, fname);	
	} else if (!memcmp(&test, &markerBytes, 4)) {
		// it is a flat file, handle that
		io = lzzCreateFileIO(context, fname, "r");
	} else 
		return _lzzReturnErrorArchive(context, "Open failed: Unknown file format.");

	arc = lzzScanIO(context, &io, method);
	return arc;
}

lzzArchive* lzzScanMemory(lzzContext *context, const char *block, unsigned long long len, int method)
{
	lzzChunk lz4MagicBytes = 	{ .byte = { 0x04, 0x22, 0x4D, 0x18 } };
	lzzChunk markerBytes = 		{ .byte = { 0x00, 0x00, 0x00, 0x00 } };
	lzzArchive *arc;
	lzzIO io;

	if (len < 44)  return _lzzReturnErrorArchive(context, "Open memory failed: Length under 44 bytes.");
	if (!memcmp(block, &lz4MagicBytes, 4)) {
		// it is an lz4 file, handle that
		io = lzzCreateLz4MemIORead(context, block, len);	
	} else if (!memcmp(block, &markerBytes, 4)) {
		// it is a flat file, handle that
		io = lzzCreateMemIOBuffer(context, block, len, (1 << 23));
	} else 
		return _lzzReturnErrorArchive(context, "Open failed: Unknown file format.");

	arc = lzzScanIO(context, &io, method);
	return arc;
}

// ------------------------------------------------
// this is the main read function, since if you pass LZZ_READ_FULL as the method it does just that
//
//		called with LZZ_READ FULL:
//			read all the contents into the ram archive
//		called with LZZ_READ_NORMAL:
//			read all INFO and TAG contents, skip DATA for now
//		called with LZZ_READ_MINIMAL:
//			read core INFO blocks, title TAG only, no DATA for now
//
//		keep in mind lzz are solid archives, so you can't seek,
//		so unless you are trying to save ram, just read it all
lzzArchive* lzzScanIO(lzzContext *context, lzzIO *io, int method)
{
	lzzChunk stopBytes = { .byte = { 0x04, 0x00, 0x00, 0x00 } };
	lzzArchive *arc;
	arc = _lzzCalloc(context, sizeof(lzzArchive), "lzzScanFile() struct allocation failed.");
	if (context->mutex) {
		arc->safety = context->mutex();
		context->lock(arc->safety);
	}
	// start with a feeble 8 entries, we can realloc this later if needed
	arc->table.entry = _lzzCalloc(context, sizeof(lzzEntry) * 8, "lzzScanFile() struct allocation failed.");
	arc->table.count = 0;
	arc->table.max = 8;
	// scan the archive using the supplied io!
	char buffer[128];
	lzzBlock block;
	lzzParserState state;
	unsigned int read;
	unsigned int ui32;
	unsigned long long ui64;
	state.markerId = -1;
	while(1) {
		lzzChunk base;
		read = _lzzIoGetNextBlock(context, arc, &state, &block, io, method);
		if (read == 0) break;	// all done, nothing more to read
		if (read == 1 && (!memcmp(block.chunk, &stopBytes, 4))) break;	// STOP!
		base = block.chunk[0];
		// always handle a marker byte
		if (base.byte[0] == 0) {
			// a marker
			ui32 = base.byte[1] + ((unsigned int)base.byte[2] << 8) + ((unsigned int)base.byte[3] << 16);
			if (ui32 != (state.markerId + 1)) {
				// error! (fatal, misformed archive)
				snprintf(buffer,128,"[%llX] Misformed archive missed expected marker %X", state.pos, (state.markerId + 1));
				buffer[127] =0;
				_lzzAddArchiveError(context, arc, buffer);
				return arc;
			}
			// TODO: finalize last marker entry

			// move to the new marker entry
			state.markerId = ui32;
			
			if (state.markerId > arc->table.count) {
				// resize the array to contain this
				if (arc->table.max < 1024) {
					ui32 = arc->table.max * 2;
				} else {
					ui32 = arc->table.max + 1024;
				}
				arc->table.entry = _lzzRealloc(context, arc->table.entry, sizeof(lzzEntry) * ui32, sizeof(lzzEntry) * arc->table.max, "lzzScanFile() entry table allocation failed.");
				arc->table.max = ui32;
			}
			// initialize a block array for this entry
			arc->table.entry[state.markerId].array.block = _lzzCalloc(context, sizeof(lzzEntry) * 8, "lzzScanFile() entry array allocation failed.");
			arc->table.entry[state.markerId].array.count = 0;
			arc->table.entry[state.markerId].array.count = 8;
			continue;
		}
		if (read > 0) {
			// parse the chunk
			switch (block.chunk[0].byte[0]) {
				case 1:
				break;
				case 2:
				break;
				case 3:
				break;
				case 4:
				break;
			}
		}
	}

	if (context->mutex) context->unlock(arc->safety);
	return arc;
}

