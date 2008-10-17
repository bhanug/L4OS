#include <sos/globals.h>
#include <sos/sos.h>
#include <stdio.h>

#define FILES 4
#define WRITE_SIZE 4096
#define DATA_SIZE 1024
#define FILENAME ".stressvfs"

int main(int argc, char **argv) {
	fildes_t fds[FILES];
	char *filenames[FILES];
	int pid = my_id();

	for (int i = 0; i < FILES; i++) {
		filenames[i] = (char *) malloc(sizeof(char) * (sizeof(FILENAME) + 8));
		snprintf(filenames[i], sizeof(FILENAME) + 8, "%s_%d_%d", FILENAME, pid, i);
		fds[i] = open(filenames[i], FM_READ | FM_WRITE);
		if (fds[i] < 0) {
			printf("%s cannot be opened: %s\n", filenames[i], sos_error_msg(fds[i]));
			exit(EXIT_FAILURE);
		}
	}

	char *data = malloc(sizeof(char) * DATA_SIZE);
	char *data2 = malloc(sizeof(char) * DATA_SIZE);

	int nw =0, nr = 0, rw = 0;
	while (1) {
		/* Write a lot */
		for (int i = 0; i < FILES; i++) {
			nw = 0;
			while ((rw = write(fds[i], data, DATA_SIZE)) > 0) {
				nw += rw;
				if (nw >= WRITE_SIZE) {
					break;
				}
			}
			if (rw < 0) {
				printf("stressvfs: Error on write (%s) (%d) (%d) (%s)\n",
						filenames[i], fds[i], rw, sos_error_msg(rw));
				exit(EXIT_FAILURE);
			}
		}

		/* Read it back */
		for (int i = FILES - 1; i >= 0; i--) {
			nr = 0;
			while ((rw = read(fds[i], data2, DATA_SIZE)) > 0) {
				nr += rw;
				if (nr >= WRITE_SIZE) {
					break;
				}
			}
			if (rw < 0 && rw != SOS_VFS_EOF) {
				printf("stressvfs: Error on read (%s) (%d) (%d) (%s)\n",
						filenames[i], fds[i], rw, sos_error_msg(rw));
				exit(EXIT_FAILURE);
			}
		}
	}

}

