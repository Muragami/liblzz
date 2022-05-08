/*
	Simple command line application to write and read .lzz
*/

#include <stdio.h>
#include <stdlib.h>
#include "lzz.h"

int plzz(const char *opts, const char *fdir, const char *name)
{
	char fbuff[512];
	if (opts[1] == '+') {
		// pack an archive
		snprintf(fbuff, 512, "%s.lzz", name);
	} else if (opts[1] == '-') {
		// unpack an archive
	} else if (opts[1] == '?') {
		// just report all the archive entries
	}
	return 0;
}

int main(int argc, const char **argv)
{
	if (argc != 3) {
		printf("plzz usage: plzz <commands> <dir/file> <?name>\n");
		return -1;
	}

	return plzz(argv[2], argv[3], argv[4]);
}