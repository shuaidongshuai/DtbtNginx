#include "common.h"
#include "Processpool.h"

int serverfd[MAX_SERVER_NUM] = {-1};//保存服务器ip
map<int, int> keep_session;

class Server1
{
public:
	int process(int sockfd)
	{
		char buf[1024];
		int nread = 0;
		int nwrite = 0;
		int n;
		
		while ((n = read(sockfd, buf + nread, 1024)) > 0) 
		{
			nread += n;
		}
		if (nread == -1 && errno != EAGAIN) 
		{
			//perror("read error");
			close(sockfd);//---------------心跳机制
			return 0;
		}
		if (nread == 0) 
		{
			printf("客服端退出\n");
			return 0;
		}
		
		buf[nread] = 0;
		printf("Server 收到：%s",buf);
		
		n = nread;
		while (n > 0)
		{
			nwrite = write(sockfd, buf + nread - n, n);//从 data_size - n 处开始读 读n个
			n -= nwrite;
		}
	}
};

int main()
{
	struct sockaddr_in servaddr, cliaddr;
	int listenfd, connfd, readLen, cliLen;
	char buf[1024], str[50];
	
	if((listenfd = socket(AF_INET,SOCK_STREAM,0)) < 0)
	{
		printf("创建socket失败\n");
		exit(0);
	}
	
	 /*一个端口释放后会等待两分钟之后才能再被使用，SO_REUSEADDR是让端口释放后立即就可以被再次使用。*/
    int reuse = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        return -1;
    }

	bzero(&servaddr,sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	inet_pton(AF_INET,SERVER_IPADDR,&servaddr.sin_addr);
	servaddr.sin_port = htons(8004);
	if(bind(listenfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) < 0)
	{
		printf("绑定失败\n");
		exit(0);
	}
	if(listen(listenfd,SOMAXCONN)<0){
		printf("监听失败\n");
		return 0;
	}
	
	Processpool* pool = Processpool::createPool( listenfd , 1);
    if( pool )
    {
        pool->run<Server1>();
        delete pool;
    }
	close(listenfd);
	return 0;
}