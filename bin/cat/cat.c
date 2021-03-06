#include <sos/globals.h>
#include <sos/sos.h>
#include <stdio.h>

#define cat_verbose 1

int main(int argc, char *argv[]) {
	fildes_t fd;
	char buf[IO_MAX_BUFFER];
	int num_read, num_written = 0, read_tot = 0, write_tot = 0;
	int ret = 0;

	if (argc != 2) {
		printf("Usage: cat filename\n");
		return 1;
	}

	fd = open(argv[1], FM_READ);
	if (fd < 0) {
		printf("%s cannot be opened\n", argv[1]);
		return 1;
	}

	//printf("<%s>\n", argv[1]);

	while ((num_read = read(fd, buf, IO_MAX_BUFFER - 1)) > 0 ) {
		buf[num_read] = '\0';
		// use write instead of printf as printf has some bugs with
		// printing weird chars
		if ((num_written = write(stdout_fd, buf, num_read)) < 0) {
			break;
		}
		read_tot += num_read;
		write_tot += num_written;
	}

	close(fd);

	if (num_read != SOS_VFS_EOF && num_read < 0) {
		printf("cat failed: error on read (%d)\n", num_read);
		printf("Can't read file: %s\n", sos_error_msg(num_read));
		kprint("error on read\n" );
		ret = 1;
	}

	if (num_written < 0) {
		printf("cat failed: error on print (%d)\n", num_written);
		printf("Can't print file: %s\n", sos_error_msg(num_written));
		kprint("error on write\n" );
		ret = 1;
	}

	if (cat_verbose > 1) {
		sprintf(buf, "Total Read: %d, Total Write: %d\n", read_tot, write_tot);
		printf("%s", buf);
		kprint(buf);
	}

	return ret;
}

