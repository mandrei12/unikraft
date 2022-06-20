#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
	
#include <unistd.h>

/* 
	not clear if normal or not but if we read from an
	invalid descriptor it just reads from the last
	succesfully used descriptor
 */
int main()
{
	char buf[200];
	int fd1, fd2, fd3, fd4, fd5;

	fd1 = open("/proc/meminfo", O_RDONLY /* | O_CREAT */);
	printf("fd1 = %d\n", fd1);
	if (fd1 > 0) {
		printf("/proc/meminfo:\n");
		read(fd1, buf, 200);
		printf("%s", buf);
	}


	/* oddly enough pare ca imi deschide acelasi fd pt un fisier inexistent */
	/* 
	 * also make sure you guard the function in boot.c with 
	 * ifdef/endif clauses
	 */
	fd2 = open("/proc/cmdline", O_RDONLY /* | O_CREAT */);
	printf("fd2 = %d\n", fd2);
	if (fd2 > 0) {
		printf("\n/proc/cmdline:\n");
		read(fd2, buf, 200);
		printf("%s", buf);
	}

	fd3 = open("/proc/version", O_RDONLY /* | O_CREAT */);
	printf("fd3 = %d\n", fd3);
	if (fd3 > 0) {
		printf("\n/proc/version:\n");
		read(fd3, buf, 200);
		printf("%s", buf);
	}

	fd4 = open("/proc/filesystems", O_RDONLY /* | O_CREAT */);
	printf("fd4 = %d\n", fd4);
	if (fd4 > 0) {
		printf("\n/proc/filesystems:\n");
		read(fd4, buf, 200);
		printf("%s", buf);
	}

	fd5 = open("/proc/mounts", O_RDONLY /* | O_CREAT */);
	printf("fd5 = %d\n", fd5);
	if (fd5 > 0) {
		printf("\n/proc/mounts:\n");
		read(fd5, buf, 200);
		printf("%s", buf);
	}
	return 0;	
}
