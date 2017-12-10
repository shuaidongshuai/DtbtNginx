#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <arpa/inet.h>

#define MAXLINE 100
#define IPADDR "127.0.0.1"
#define SERV_PORT 8000

int main()
{
	struct sockaddr_in servaddr;
	char buf[MAXLINE],num;
	int sockfd;
	
	if((sockfd = socket(AF_INET,SOCK_STREAM,0)) < 0)
	{
		printf("创建socket失败\n");
		exit(0);
	}
	bzero(&servaddr,sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	inet_pton(AF_INET,IPADDR,&servaddr.sin_addr);
	servaddr.sin_port = htons(SERV_PORT);
	
	if(connect(sockfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) == -1)
	{
		printf("连接失败\n");
		return 0;
	}
	
	while(fgets(buf,MAXLINE,stdin)!=NULL)
	{
		write(sockfd,buf,strlen(buf));
		num = read(sockfd,buf,MAXLINE);
		if(0 == num)
		{
			printf("服务器关闭\n");
			return 0;
		}
		else
			write(STDOUT_FILENO,buf,num);
	}
	close(sockfd);
	return 0;
}