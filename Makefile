all:
	gcc -Wall -Wextra -o tftp_client tftp_client.c

clean:
	rm -f tftp_client