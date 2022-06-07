#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

int main()
{
	int fd1 = open("meminfo", O_WRONLY /* | O_CREAT */);
	char buf[200];	

		
	fd1 = open("meminfo", O_RDONLY);

	buf[0] = '\0';
	read(fd1, buf, 200);
	printf("%s", buf);

	close(fd1);
	close();

	return 0;
	
}
