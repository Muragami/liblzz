/*
	see lzz.h for details
*/

#include "lzz.h"
#include "include/lz4io.h"
#include "include/lz4hc.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

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
	fputs(error, stderr);
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

XIO _lzzMemWriteXIO(lzzContext *c, char *buffer, unsigned int len, unsigned int maxDouble)
{
	XIO x;
	lzzXioMemInteral *m = _lzzCalloc(c, sizeof(lzzXioMemRefInteral), "_lzzMemWriteXIO() allocation failed.");
	
	if (buffer == NULL) buffer = _lzzMalloc(c, len, "_lzzMemWriteXIO() dynamic allocation failed.");
	m->bytes = buffer;
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

lzzIO lzzCreateLz4FastMemIOWrite(lzzContext *c, char *buffer, unsigned int len, unsigned int maxDouble)
{
	lzzIO io;
	lzzLz4Interal *i;
	LZ4F_errorCode_t r;

	i = _lzzCalloc(c, sizeof(lzzLz4Interal), "lzzCreateLz4FastMemIOWrite() internal allocation failed.");
	if (maxDouble == 0) maxDouble = (1 << 23);
	i->xio = _lzzMemWriteXIO(c, buffer, len, maxDouble);
	r = LZ4FIO_writeOpen(&i->lz4fWrite, &i->xio, NULL);
	if (LZ4F_isError(r)) {
		strncpy(io.err,LZ4F_getErrorName(r),256);
		io.err[255] = 0;
	}
	i->flags = LZZ_LZ4_WRITE;
	io.read = _lzzLz4Read;
	io.write = _lzzLz4Write;
	io.done = _lzzLz4Done;

	return io;
}

lzzIO lzzCreateLz4HCMemIOWrite(lzzContext *c, char *buffer, unsigned int len, unsigned int maxDouble, int level)
{
	lzzIO io;
	lzzLz4Interal *i;
	LZ4F_errorCode_t r;
	LZ4F_preferences_t hcpref = { LZ4F_INIT_FRAMEINFO, LZ4HC_CLEVEL_DEFAULT + level, 0u, 0u, { 0u, 0u, 0u } };

	i = _lzzCalloc(c, sizeof(lzzLz4Interal), "lzzCreateLz4FastMemIOWrite() internal allocation failed.");
	if (maxDouble == 0) maxDouble = (1 << 23);
	i->xio = _lzzMemWriteXIO(c, buffer, len, maxDouble);
	r = LZ4FIO_writeOpen(&i->lz4fWrite, &i->xio, &hcpref);
	if (LZ4F_isError(r)) {
		strncpy(io.err,LZ4F_getErrorName(r),256);
		io.err[255] = 0;
	}
	i->flags = LZZ_LZ4_WRITE;
	io.read = _lzzLz4Read;
	io.write = _lzzLz4Write;
	io.done = _lzzLz4Done;

	return io;
}


int _lzzAddArchiveError(lzzContext *c, lzzArchive *a, const char *err)
{
	lzzErrors *pe = &a->e;
	if (pe->errCount == 15) return -1;
	char *dst = pe->errBuffer + pe->errCount * 128;
	strncpy(dst, err, 127);
	dst[127] = 0;
	return pe->errCount++;
}

// read the block into a temporary buffer, limited to 8kb blocks
void _lzzScanBlock(lzzParserState *state, lzzIO *io, int len, const char **err, unsigned long long *err_pos) 
{
	if (len > 8192) len = 8192;
	int rd = io->read(io, (char*)&state->buffer, len);
	if (rd != len) {
		// error!
		*err = "data chunk read invalid length";
		*err_pos = state->pos;
	} else {
		state->hash = _lzzElfHash(&state->hash, (const unsigned char*)&state->buffer, len);
		state->pos += len;
	}
}

int _lzzReadBlock(lzzContext *c, lzzParserState *state, lzzChunk *first, int len, const char **err, unsigned long long *err_pos) 
{
	lzzBlockArray *dest;
	if (state->arc->fixedArray != NULL)
		dest = state->arc->fixedArray;		// one fixed array for the whole archive
	else
		dest = &state->arc->table.entry[state->markerId].array;
									// one allocated array per entry
	
	unsigned int oldLen = dest->max;
	while (dest->count + (len >> 2) >= dest->max) {
		if (state->arc->fixedArray != NULL) return -1;	// we have reached the limit of this fixed array so bug out!
		// reallocate the array
		if (dest->max == 0) {
			dest->max = 16384;
		} else {
			if (dest->max < 2097152) 
				dest->max = dest->max << 1;
			else
				dest->max += 2097152;
		}
	}
	if (oldLen != dest->max) {
		dest->chunk = _lzzRealloc(c, dest->chunk, dest->max << 2, oldLen << 2, "_lzzReadBlock() block reallocation failed.");
	}
	
	memcpy(dest->chunk,&first,4);
	int rd = state->io->read(state->io, (char*)&dest->chunk[dest->count], len);
	if (rd != len) {
		// error!
		*err = "data chunk read invalid length";
		*err_pos = state->pos;
	} else {
		state->hash = _lzzElfHash(&state->hash, (const unsigned char*)&dest->chunk[dest->count], len);
		state->pos += len;
		dest->count += len >> 2;
	}


	return len;
}

// force 4-byte alignment for reading chunks
static inline unsigned int _lzzChunkLen(unsigned int sz)
{
	unsigned int ret = sz >> 2;
	if ((sz % 4) > 0) ret++;
	return ret << 2;
}

int _lzzEnsureBlockArray(lzzContext *c, lzzBlockArray *b, int cnt, void *fixed)
{
	int oldLen = b->max;
	while (cnt + b->count >= b->max) {
		if (fixed != NULL) return -1;
		// reallocate the array
		if (b->max == 0) {
			b->max = 16384;
		} else {
			if (b->max < 2097152) 
				b->max = b->max << 1;
			else
				b->max += 2097152;
		}
	}
	if (oldLen != b->max) {
		b->chunk = _lzzRealloc(c, b->chunk, b->max << 2, oldLen << 2, "_lzzReadBlock() block reallocation failed.");
	}
	return 0;
}

void _lzzSkipBlock(lzzParserState *state, int len, const char **err, unsigned long long *err_pos) 
{
	while (len > 0) {
		int nibble = len;
		if (nibble > 8192) nibble = 8192;
		int rd = state->io->read(state->io, (char*)&state->buffer, nibble);
		if (rd != nibble) {
			// error!
			*err = "data chunk stream read error";
			*err_pos = state->pos;
			return;
		} else {
			state->hash = _lzzElfHash(&state->hash, (const unsigned char*)&state->buffer, nibble);
			state->pos += nibble;
		}
		len -= nibble;
	}
}

// returns -1 on stream end or error, 0 if a block is skipped, or the total chunks read
int _lzzIoGetNextBlock(lzzContext *c, lzzParserState *state, int method)
{
	char buffer[128];
	const char *err = NULL;
	unsigned long long err_pos = 0; 
	int ret = 0;
	int cnt;
	lzzChunk *data = &state->entry->array.chunk[state->entry->array.count];
	lzzBlockArray *b = &state->block;

	cnt = state->io->read(state->io, (char*)data, 4);
	if (cnt != 4) return -1;	// stream has ended (hopefully!)
	state->hash = _lzzElfHash(&state->hash, (const unsigned char*)&data, 4);
	state->pos += 4;
	state->entry->array.count++;
	state->block.count = 1;
	state->block.chunk = data;	// block now points to the destination for our new block, whereever it lies!
	state->block.max = state->entry->array.max;

	// let's see what is coming
	switch (data->byte[0]) {
		case 0: // marker, nothing!
		case 4: // stop, nothing!
			// these are single chunk by definition so we are already done!
			b->count = 1;
			return 1;
		break;
		case 1: { // tag, you're it!
			unsigned int sz = (unsigned int)data->byte[1] + (unsigned int)data->byte[2];
			unsigned int len = _lzzChunkLen(sz);
			if ((method & 0xFF) != LZZ_READ_MINIMAL) {
				// read all the tags!
				ret = _lzzReadBlock(c, state, data, len, &err, &err_pos);
			} else {
				// read only 'title' tags, kind of fake we end up reading in everything anyway no matter what because
				// how else do we know the name of the tag???!
				_lzzScanBlock(state, state->io, len, &err, &err_pos);
				if (!memcmp(&state->buffer,"title",5)) {
					// ok, keep it, copy the buffer into our block array
					cnt = _lzzEnsureBlockArray(c, b, len >> 2, state->arc->fixedArray);
					if (cnt == -1) return -1;
					memcpy(&b->chunk[b->count], &state->buffer, len);
					b->count += len >> 2;
					ret = len >> 2;
				} else {
					ret = 0;
				}
			}
		}
		break;
		case 2: { // info, get news!
			unsigned int sz;
			if (data->byte[1] > 0x7F) {
 				sz = (unsigned int)data->byte[2] + ((unsigned int)data->byte[3] << 8) * 4;
 				if (sz < 8188) {
 					err = "info chunk exceeds 2047 size limit";
					err_pos = state->pos;
					break;
 				}
			} else {
				sz = 4;
			}
			if ((method & 0xFF) != LZZ_READ_MINIMAL) {
				// read the data block in
				ret = _lzzReadBlock(c, state, data, sz, &err, &err_pos);
			} else {
				// read just the core infos
				switch (data->byte[1]) {
					case 0x00:
					case 0x01:
					case 0x02:
					case 0x03:
					case 0x04:
					case 0x05:
					case 0x06:
					case 0x07:
					case 0x80:
						ret = _lzzReadBlock(c, state, data, sz, &err, &err_pos);
					break;

					default:
						_lzzSkipBlock(state, sz, &err, &err_pos);
					break;
				}
			}
		}
		break;
		case 3: { // data, the good stuff!
			if (data->byte[1] > 2) {
				// error!
				err = "data chunk type byte invalid";
				err_pos = state->pos;
			} else {
				unsigned int sz = (unsigned int)data->byte[2] + ((unsigned int)data->byte[3] << 8);
				if (data->byte[1] == 2) {
					if (data->byte[2] == 1) {
						sz = 32;
					} else if (data->byte[2] == 1) {
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
					_lzzSkipBlock(state, len, &err, &err_pos);
				} else {
					// read the data block in
					ret = _lzzReadBlock(c, state, data, len, &err, &err_pos);
				}
			}
		}
		break;

		default: // custom chunk, see if we ignore or callback
			if (c->custom[data->byte[0]]) {
				unsigned int sz = (unsigned int)data->byte[2] + ((unsigned int)data->byte[3] << 8);
				unsigned int len = _lzzChunkLen(sz);
				int rd = c->custom[data->byte[0]](state->arc, data, NULL, sz);
				if (rd > 0) {
					if (rd * sizeof(lzzChunk) > len) {
						// error!
						err = "custom chunk read request to long";
						err_pos = state->pos;
					} else {
						ret = _lzzReadBlock(c, state, data, len, &err, &err_pos);
						c->custom[data->byte[0]](state->arc, data, &b->chunk[1], sz);
					}
				}
			}

		break;
	}

	if (err != NULL) {
		snprintf(buffer,128,"[%llX] %s",err_pos,err);
		buffer[127] =0;
		_lzzAddArchiveError(c, state->arc, buffer);
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

	ret->customLimit = 4096;
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

void lzzMakeMemoryContextFixed(lzzContext *context, unsigned int blocks, unsigned int entries)
{
	context->blocksFixed = blocks;
	if (entries == 0) entries = 800;
	context->entriesFixed = entries;
}

void lzzMakeMemoryContextDynamic(lzzContext *context)
{
	context->blocksFixed = 0;
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

	return lzzScanIO(context, &io, method);
}

lzzArchive* lzzScanMemory(lzzContext *context, const char *block, unsigned long long len, int method)
{
	lzzChunk lz4MagicBytes = 	{ .byte = { 0x04, 0x22, 0x4D, 0x18 } };
	lzzChunk markerBytes = 		{ .byte = { 0x00, 0x00, 0x00, 0x00 } };
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

	return lzzScanIO(context, &io, method);
}

void _lzzParseArchiveBlock(lzzContext *context, lzzParserState *state)
{
	// TODO!
	lzzBlockArray *b = &state->block;
	switch (b->chunk[0].byte[0]) {
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

// ------------------------------------------------
/*
	Fixed sized archives are in memory as a block like so:

		lzzArchive struct
		lzzBlockArray struct
		[ALLOCATED CHUNKS]

	Fixed also allocated a separate entry index table.
*/
lzzArchive* lzzEmptyArchive(lzzContext *context)
{
	lzzArchive* ret;
	if (context->blocksFixed != 0) {
		// we allocate one large brick of chunks to use for this archive, blocksFixed is the amount
		ret = _lzzCalloc(context, sizeof(lzzArchive) + sizeof(lzzBlockArray) + (context->blocksFixed << 2), "lzzEmptyArchive() fixed allocation failed.");
		if (context->mutex) ret->safety = context->mutex();
		ret->user = context->user;
		// now configure the archive itself with the fixed addresses
		ret->fixedArray = (lzzBlockArray*)(((char*)ret) + sizeof(lzzArchive));
		ret->fixedArray->max = context->blocksFixed;
		ret->fixedArray->chunk = (lzzChunk*)(((char*)ret->fixedArray) + sizeof(lzzBlockArray));
		// we allocate one entry array for our contents, up to entriesFixed max
		ret->table.entry = (lzzEntry*)_lzzCalloc(context, sizeof(lzzEntry) * context->entriesFixed, "lzzEmptyArchive() fixed entry allocation failed.");
		ret->table.max = context->entriesFixed;
	} else {
		// dynamic archive, so allocate entries on demand and trust in realloc()
		ret = _lzzCalloc(context, sizeof(lzzArchive), "lzzEmptyArchive() dynamic struct allocation failed.");
		if (context->mutex) ret->safety = context->mutex();
		ret->user = context->user;
		// start with a feeble 32 entries, we can realloc this later if needed
		ret->table.entry = _lzzCalloc(context, sizeof(lzzEntry) * 32, "lzzScanFile() struct allocation failed.");
		ret->table.count = 0;
		ret->table.max = 32;
	}
	return ret;
}

lzzArchive* lzzScanIO(lzzContext *context, lzzIO *io, int method)
{
	lzzArchive *arc = lzzEmptyArchive(context);
	lzzScanIOInto(context, arc, io, method);
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
void lzzScanIOInto(lzzContext *context, lzzArchive *arc, lzzIO *io, int flags)
{
	// or not, so let's just allocate however the hell we please
	lzzChunk stopBytes = { .byte = { 0x04, 0x00, 0x00, 0x00 } };
	if (context->mutex) {
		arc->safety = context->mutex();
		context->lock(arc->safety);
	}
	// scan the archive using the supplied io!
	char buffer[128];
	lzzParserState state;
	lzzBlockArray *blk = &state.block;
	unsigned int read;
	unsigned int ui32;
	// initialize the state, just a bit
	state.io = io;
	state.entry = NULL;
	state.arc = arc;
	state.markerId = -1;

	while(1) {
		lzzChunk base;
		read = _lzzIoGetNextBlock(context, &state, flags);
		if (read == 0) break;	// all done, nothing more to read
		if (read == 1 && (!memcmp(blk->chunk, &stopBytes, 4))) break;	// STOP!
		base = blk->chunk[0];
		// always handle a marker byte
		if (base.byte[0] == 0) {
			// a marker
			ui32 = base.byte[1] + ((unsigned int)base.byte[2] << 8) + ((unsigned int)base.byte[3] << 16);
			if (ui32 != (state.markerId + 1)) {
				// error! (fatal, misformed archive)
				snprintf(buffer,128,"[%llX] Misformed archive missed expected marker %X", state.pos, (state.markerId + 1));
				buffer[127] = 0;
				_lzzAddArchiveError(context, arc, buffer);
				if (context->mutex) context->unlock(arc->safety);
				return;
			}
			// TODO: finalize last marker entry

			// move to the new marker entry
			state.markerId = ui32;
			
			if (state.markerId > arc->table.count) {
				// is this a fixed archive? then error out
				if (arc->fixedArray != NULL) {
					snprintf(buffer,128,"[%llX] Too many entries for this fixed archive %d", state.pos, (state.markerId + 1));
					buffer[127] = 0;
					_lzzAddArchiveError(context, arc, buffer);
					if (context->mutex) context->unlock(arc->safety);
					return;
				}
				// resize the array to contain this
				if (arc->table.max < 1024) {
					ui32 = arc->table.max * 2;
				} else {
					ui32 = arc->table.max + 1024;
				}
				arc->table.entry = _lzzRealloc(context, arc->table.entry, sizeof(lzzEntry) * ui32, sizeof(lzzEntry) * arc->table.max, "lzzScanIOInto() entry table allocation failed.");
				arc->table.max = ui32;
			}

			if (arc->fixedArray != NULL) {
				// it is a fixed array, so map our new entry into the fixed chunk array
				arc->table.entry[state.markerId].array.chunk = &arc->fixedArray->chunk[arc->fixedArray->count];
				arc->table.entry[state.markerId].array.count = 0;
				arc->table.entry[state.markerId].array.max = arc->fixedArray->max - arc->fixedArray->count;
			} else {
				// allocate a new array
				arc->table.entry[state.markerId].array.chunk = _lzzCalloc(context, 65536, "lzzScanIOInto() entry chunk allocation failed.");
				arc->table.entry[state.markerId].array.max = 65536 >> 2;
				arc->table.entry[state.markerId].array.count = 0;
			}
			// set the current entry pointer and insert the marker chunk first
			state.entry = &arc->table.entry[state.markerId];
			state.entry->array.chunk[0] = base;
			state.entry->array.count = 1;
			continue;
		}
		if (read > 0) {
			// parse the chunk
			_lzzParseArchiveBlock(context, &state);
		}
	}
	if (context->mutex) context->unlock(arc->safety);
}

void lzzScanFileInto(lzzContext *context, lzzArchive *arc, const char *fname, int flags)
{
	lzzChunk lz4MagicBytes = { .byte = { 0x04, 0x22, 0x4D, 0x18 } };
	lzzChunk markerBytes = { .byte = { 0x0, 0x0, 0x0, 0x0 } };
	lzzChunk test;
	lzzIO io;
	FILE *fp;

	fp = fopen(fname,"r");
	if (fp == NULL) {
		_lzzAddArchiveError(context, arc, "lzzScanFileInto() fopen() failed.");
		return;
	}
	fread(&test, 1, 4, fp);
	fclose(fp);

	if (!memcmp(&test, &lz4MagicBytes, 4)) {
		// it is an lz4 file, handle that
		io = lzzCreateLz4FileIORead(context, fname);	
	} else if (!memcmp(&test, &markerBytes, 4)) {
		// it is a flat file, handle that
		io = lzzCreateFileIO(context, fname, "r");
	} else  {
		_lzzAddArchiveError(context, arc, "lzzScanFileInto() invalid file format.");
		return;
	}
	lzzScanIOInto(context, arc, &io, flags);
}

void lzzScanMemoryInto(lzzContext *context, lzzArchive *arc, const char *block, unsigned long long len, int flags)
{
	lzzChunk lz4MagicBytes = 	{ .byte = { 0x04, 0x22, 0x4D, 0x18 } };
	lzzChunk markerBytes = 		{ .byte = { 0x00, 0x00, 0x00, 0x00 } };
	lzzIO io;

	if (len < 44) {
		_lzzAddArchiveError(context, arc, "lzzScanMemoryInto() memory block length under 44 bytes.");
		return;
	} 
	if (!memcmp(block, &lz4MagicBytes, 4)) {
		// it is an lz4 file, handle that
		io = lzzCreateLz4MemIORead(context, block, len);	
	} else if (!memcmp(block, &markerBytes, 4)) {
		// it is a flat file, handle that
		io = lzzCreateMemIOBuffer(context, block, len, (1 << 23));
	} else {
		_lzzAddArchiveError(context, arc,"lzzScanMemoryInto() invalid file format.");
		return;
	}

	lzzScanIOInto(context, arc, &io, flags);
}

unsigned long long lzzWriteFile(lzzContext *context, lzzArchive *arc, int mode, const char *fname)
{
	lzzIO io;

	switch (mode) {
		case LZZ_MODE_FLAT:
			io = lzzCreateFileIO(context, fname, "w");
		break;
		case LZZ_MODE_FAST:
			io = lzzCreateLz4FastFileIOWrite(context, fname);
		break;
		case LZZ_MODE_HC:
			io = lzzCreateLz4HCFileIOWrite(context, fname, 0);
		break;
	}

	return lzzWriteIO(context, arc, mode, &io);
}

unsigned long long lzzWriteMemory(lzzContext *context, lzzArchive *arc, int mode, char **data)
{
	lzzIO io;

	switch (mode) {
		case LZZ_MODE_FLAT:
			io = lzzCreateMemIOBuffer(context, NULL, arc->totalBytes, (1 << 23));
		break;
		case LZZ_MODE_FAST:
			io = lzzCreateLz4FastMemIOWrite(context, NULL, arc->totalBytes, (1 << 23));
		break;
		case LZZ_MODE_HC:
			io = lzzCreateLz4HCMemIOWrite(context, NULL, arc->totalBytes, (1 << 23), 0);
		break;
	}

	return lzzWriteIO(context, arc, mode, &io);
}

unsigned long long lzzWriteIO(lzzContext *context, lzzArchive *arc, int mode, lzzIO *io)
{
	if (arc->fixedArray != NULL) {
		// easy peasy, we already have a binary block in memory to handle
		return io->write(io, (char*)arc->fixedArray->chunk, arc->fixedArray->count << 2);
	} else
	{
		// still not bad, loop over every entry and write it out in order
		unsigned long long ret = 0;
		unsigned int i = 0;
		while (i < arc->table.count) {
			lzzEntry *e = &arc->table.entry[i];
			unsigned int len = e->array.count << 2;
			unsigned int w = io->write(io, (char*)e->array.chunk, len);
			if (w != len) return -1;
			ret += e->array.count << 2;
			i++;
		}
		return ret;
	}
}

int lzzAddFolder(lzzContext *context, lzzArchive *arc, const char *title)
{

}

int lzzAddEntry(lzzContext *context, lzzArchive *arc, const char *title, const char *extension, const char *mime, unsigned int uid, const char *data, unsigned long long data_length)
{

}

int lzzSetFetcher(lzzContext *context, lzzArchive *arc, LZZFETCH f)
{

}

