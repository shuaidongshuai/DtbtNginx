#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#define MAXLINE 100
#define IPADDR "127.0.0.1"
#define SERV_PORT 8000

char buf[MAXLINE] = "hello dong";
int m = 0;
void* runProcess(void *n)
{
	struct sockaddr_in servaddr;
	int sockfd;
	char num;
	
	if((sockfd = socket(AF_INET,SOCK_STREAM,0)) < 0)
	{
		printf("创建socket失败\n");
		return NULL;
	}
	bzero(&servaddr,sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	inet_pton(AF_INET,IPADDR,&servaddr.sin_addr);
	servaddr.sin_port = htons(SERV_PORT);
	
	if(connect(sockfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) == -1)
	{
		printf("连接失败\n");
		return NULL;
	}
	
	write(sockfd,buf,strlen(buf));
	char buff[30];
	num = read(sockfd,buff,MAXLINE);
	if(0 == num)
	{
		printf("服务器关闭\n");
		return NULL;
	}
	buff[num] = 0;
	printf("%s %d\n",buff,m++);
	
	close(sockfd);
}

// int main2()
// {
	// pid_t pid;
	
	// for(int i = 0; i < 10; i++)
	// {
		// pid = fork();
		// if(pid < 0)
		// {
			// perror("fork失败！！");
		// }
		// else if( pid == 0 )
		// {
			// runProcess(NULL);
			// return 0;
		// }
		// printf("%d\n",i);
	// }
	// sleep(100);
	
	// return 0;
// }
int main()
{
	pthread_t pt1;
	for(int i = 0; i < 1000;i++)
	{
		pthread_create(&pt1,NULL,runProcess,NULL);
	}
	sleep(50);
}