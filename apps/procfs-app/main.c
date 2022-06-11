#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
	
#include <unistd.h>


int main()
{
	char buf[200];	
	int fd1 = open("meminfo", O_RDONLY /* | O_CREAT */);
	
	
	read(fd1, buf, 200);
	printf("%s", buf);

	int fd3 = open("version", O_RDONLY /* | O_CREAT */);
	read(fd3, buf, 200);
	printf("%s", buf);


	close(fd1);
	close(fd3);


	return 0;
	
}
