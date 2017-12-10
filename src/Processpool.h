#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h> 
#include <sys/socket.h>
#include <assert.h>
#include <signal.h>
#include <netinet/tcp.h> 
#include <iostream>
#include <new>
#include "easyloggingpp/easylogging++.h"
#include "DtbtNginx.h"
#include "Nginx.h"
#include "ConsistentHash.h"
using namespace std;

class Processpool
{
private:
	/*描述一个子进程的类*/
	class process
	{
	public:
	    process() : pid( -1 ){}
	public:
	    pid_t pid;//子进程的PID
	    int pipefd[2];//父进程和子进程通信用的管道
	};
private:
	/*保存所有子进程的描述信息*/
    process *subProcess;
	/*进程允许的最大子进程数量*/
    static const int MAX_processNumber = 16;
    /*每个子进程最多能处理的客户数量*/
    static const int USER_PER_PROCESS = 65536;
    /* 主线程监听的事件 ≈ 集群个数  主线程不同于子线程，监听事件少 */
    static const int MAX_MAIN_PROCESS_EVENT = 100;
    /*epoll最多能处理的事件数*/
    static const int MAX_EVENT = 65536;
    /*进程池中的进程总数*/
    int processNumber;
    /*子进程在池中的序号，从0开始*/
	int childIdx;
    /*每个进程都有一个epoll内核事件表，用epoolfd标识*/
    int epollfd;
    /*监听socket*/
    int listenfd;
    /* 是否主进程来accept */
    bool isFatherAccept;//false 那么当前进程需要继续往下执行，创建新的进程来担当父进程
    /*子进程通过stop来决定是否停止运行*/
    int stop;
    /* 信号管道 */
    static int sigPipefd[2];
protected:
	/*发生信号时的动作*/
	static void sig_handler( int sig );
	/*注册信号*/
	void addsig( int sig, void( handler )(int), bool restart = true );
	/*安装信号 创建epoll事件监听表和信号管道*/
	void InitSigPipe(Nginx *nginxs);
	/*运行父进程*/
	template< class T >
    void runParent();
	/*运行子进程*/
	template< class T >
    void runChild();
	/*设置非阻塞*/
	int SetNoBlocking(int fd);
	/*epoll内核事件表中删除fd上的所有注册事件*/
	void removefd( int epollfd, int fd );
public:
	/*根据监听套接字创建进程池 默认有4个子进程*/
	Processpool(int processNumber);
	~Processpool();
	static Processpool *CreateProcesspool( int processNumber, bool isFatherAccept );
	/*运行一个父进程或一个子进程*/
	template<class T>
	void run();
};
int Processpool::sigPipefd[2] = {0};

Processpool *Processpool::CreateProcesspool(int processNumber, bool isFatherAccept){
	/* isFatherAccept 这个参数是预防将来父进程不进行accept而去干别的事而提出来的 */
	if(isFatherAccept){
		return new Processpool(processNumber);
	}
	pid_t pid = fork();
	if(0 == pid){
		return NULL;//如果当前进程不是父进程，那么返回空
	}
	else if(pid > 0){
		return new Processpool(processNumber);
	}
	else{
		LOG(ERROR) << "CreateProcesspool fork Error";
	}
}

Processpool::Processpool(int processNumber):processNumber(processNumber) {
	if(processNumber < 0 || processNumber > MAX_processNumber) {
		LOG(ERROR) << "Processpool ";
		return;
	}
	/* 父进程编号 子进程是否停止运行 */
	childIdx = -1;//进来是父进程 -1
	stop = false;
	
	/*创建用于保存所有子进程的描述信息的指针*/
	subProcess = new process[processNumber];
	assert( subProcess );
	
	/*创建processNumber个子进程，并建立他们和父进程之间的通信管道*/
	for(int idx = 0; idx < processNumber ; idx++) {
		/*还没创建子进程之前 父进程和预定的每一个子进程都先建立一个管道，到时候可以分开通知某一个子进程*/
		int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, subProcess[idx].pipefd);
		assert( ret == 0 );
		
		subProcess[idx].pid = fork();//创建子进程，并保存子进程id
		assert( subProcess[idx].pid >= 0 );
		 
		if( subProcess[idx].pid > 0 ) {
            close( subProcess[idx].pipefd[1] );//父进程用 pipefd[0] 进程通信
			/* 设置通信管道非阻塞 because : epoll ET mode*/
			if(SetNoBlocking(subProcess[idx].pipefd[0]) < 0) {
				LOG(WARNING) << "SetNoBlocking 失败 父subProcess["<< idx <<"].pipefd[0] = " << subProcess[idx].pipefd[0];
			}
            continue;
         }
        else if(subProcess[idx].pid == 0) {
            close( subProcess[idx].pipefd[0] );//子进程用 pipefd[1] 进程通信
			if(SetNoBlocking(subProcess[idx].pipefd[1]) < 0)	{
				LOG(WARNING) << "SetNoBlocking 失败 子subProcess["<< idx <<"].pipefd[0] = " << subProcess[idx].pipefd[0];
			}
            childIdx = idx;
            break;
         }
		 else {
		 	LOG(WARNING) << "创建子进程失败";
		 }
	}
}

Processpool::~Processpool() {
	delete [] subProcess;
}

int Processpool::SetNoBlocking(int fd) {
	int old_option = fcntl(fd,F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	return fcntl(fd, F_SETFL, new_option);//如果出错，所有命令都返回-1
}

void Processpool::removefd( int epollfd, int fd ) {
	epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,NULL);
	close(fd);
}

void Processpool::sig_handler( int sig ) {
    send( sigPipefd[1], ( char * )&sig, 1, 0 );				//向sigPipefd[1]发送一个字节的信号
}

void Processpool::addsig( int sig, void( handler )(int), bool restart) {
	struct sigaction sa;
	memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;									//信号发生时执行的函数
	if( restart ) {
        sa.sa_flags |= SA_RESTART;								//SA_RESTART: 由此信号中断的系统调用会自动重启
    }
    sigfillset( &sa.sa_mask );									//所有的信号加入到此信号集里
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

void Processpool::InitSigPipe(Nginx *nginxs) {
	/* 不是父子间通信了，而是系统和进程间的通信*/
    int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sigPipefd );//初始化信号通道
    assert( ret != -1 );
	
	//在同一个进程中也可以进行通信，向s[0]中写入，就可以从s[1]中读取（只能从s[1]中读取）
	//也可以在s[1]中写入，然后从s[0]中读取
	//但是，若没有在0端写入，而从1端读取，则1端的读取操作会阻塞，所以设置非阻塞
    if(SetNoBlocking( sigPipefd[1] ) < 0) {
		LOG(WARNING) << "SetNoBlocking 失败";
	}
	nginxs[sigPipefd[0]].sockfd = sigPipefd[0];
	nginxs[sigPipefd[0]].Addfd2Read();

	/* 设置信号处理函数 */
    addsig( SIGCHLD, sig_handler );//子进程结束信号 
    addsig( SIGTERM, sig_handler );//终止信号 
    addsig( SIGINT, sig_handler );//键盘中断
    addsig( SIGPIPE, SIG_IGN );//SIGPIPE管道破裂（写一个没有读端口的管道 ）SIG_IGN：忽略SIGPIPE信号
}

/*create_pool之后（父子进程都已创建） 接着父子就会调用这个run*/
template< class T >
void Processpool::run() {
    if( childIdx != -1 ) {
        runChild< T >();
        return;
    }
    runParent<T>();
}
/* 子进程需要先解析数，看给哪个服务器集群，然后根据一致性hash选择其中一台服务器 */
template< class T >
void Processpool::runChild() {
	/* 创建epoll事件监听表 */
	epollfd = epoll_create(MAX_EVENT);
	if(-1 == epollfd){
		LOG(ERROR) << "runChild epoll_create";
		return;
	}

	/* 事件处理Nginx fd既为角标 相当于hash表 减少了查找的时间*/
	Nginx *nginxs;
	try {
		nginxs = new Nginx[MAX_EVENT + 1];
	}
	catch(const bad_alloc& e)	{
		LOG(ERROR) << "Nginx new error : " << e.what();
		return;
	}
	for(int i = 0; i < MAX_EVENT + 1; ++i){
		nginxs[i].epollfd = epollfd;
	}
	
	/* 创建信号pipe */
	InitSigPipe(nginxs);

	/* 添加Nginxs的起始地址 */
	DtbtNginx *dbNginx = Singleton<DtbtNginx>::getInstence();
	dbNginx->nginxs = nginxs;

	/* 每个子进程都通过其在进程池中的序号值childIdx找到与父进程通信的管道 */
    int pipefd = subProcess[childIdx].pipefd[ 1 ];
    nginxs[pipefd].sockfd = pipefd;
    nginxs[pipefd].Addfd2Read();
	
	epoll_event events[ MAX_EVENT ];
    int number = 0;
    int ret = -1;
	int ParMessage = 0;
	int sockfd, connfd;
	int readSize = 0;
	struct sockaddr_in client_addr;
	socklen_t client_addrlen = sizeof(client_addr);
	string ipStr, portStr;

	/* 小根堆 用于 keepalive , 将来如果某个事件需要特殊处理也可以加进来 */
	dbNginx->TimeHeapAdd(2000);//心跳的间隔为2秒

	while( ! stop )	{
		number = epoll_wait( epollfd, events, MAX_EVENT, dbNginx->TimeHeapGet());
		//EINTR由于信号中断 产生的 我们一般会重新执行该系统调用
		if ( ( number < 0 ) && ( errno != EINTR ) ) {
            LOG(ERROR) << "epoll_wait < 0";
        	break;
        }
		for( int i = 0; i < number ; i++) {
			sockfd = ((Nginx *)events[i].data.ptr)->sockfd;
			 /* 接收父进程发过来的信息 */
			if( ( sockfd == pipefd ) && ( events[i].events & EPOLLIN ) ) {
				if(nginxs[sockfd].sockfd == -1){
            		LOG(ERROR) << "Read sockfd had close fd=" << sockfd;
            	}
				else if(nginxs[sockfd].ReadProto()){
					nginxs[sockfd].readIdx = 0;
				}
			}
			/* 接收到信号 */
			else if( ( sockfd == sigPipefd[0] ) && ( events[i].events & EPOLLIN ) )	{
				char signals[10];
				if(nginxs[sockfd].Read()){
					ret = nginxs[sockfd].readIdx;
					//数据读完 或者 close 读指针置0
					nginxs[sockfd].readIdx = 0;
				}
				else{
					// 没有读完
					continue;
				}
				for( int i = 0; i < ret ; i++)	{
					// LOG(DEBUG) << "child recv a signal:" << sigPipefd[i];
					switch( signals[i] ) {
						case SIGCHLD://可以不要
						{
							pid_t pid;
							int stat;
							while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )//waitpid()会暂时停止目前进程的执行，直到有信号来到或子进程结束
							{
								printf("waitpid error\n");
								continue;
							}
							wait(NULL);
							break;
						}
						case SIGTERM:
						case SIGINT: {
							stop = true;
							break;
						}
						default: break;
					}
				}
			}
			/* 服务器响应回来了 */
			else if(!dbNginx->mSerfdName[sockfd].empty() && events[i].events & EPOLLIN){
				if(nginxs[sockfd].sockfd == -1){
            		LOG(ERROR) << "Read sockfd had close fd=" << sockfd;
            	}
				else if(nginxs[sockfd].ReadProto()){
					nginxs[sockfd].readIdx = 0;
				}
			}
			/*客服端请求来到*/
			else if( events[i].events & EPOLLIN ) {
				if(nginxs[sockfd].sockfd == -1) {
            		LOG(ERROR) << "write sockfd had close fd=" << sockfd;
            	}
				//先读完请求
				if(nginxs[sockfd].Read()){
					readSize = nginxs[sockfd].readIdx;
					nginxs[sockfd].readIdx = 0;
				}
				else{
					// 没有读完
					continue;
				}
				/* 采用 Consistent Hash 算法分配给子进程 */
				string SerName = dbNginx->csshash->getServerName(nginxs[sockfd].clientName);
				int serverfd = dbNginx->mSerNamefd[SerName];
				//如果不存在需要主动和Server建立连接--waiting for you
				if(serverfd <= 0){
					LOG(ERROR) << "server dont alive : " << SerName;
				}
				else{
					//发给服务器
					string data = string(nginxs[serverfd].readBuf, readSize);
                	if(nginxs[serverfd].WriteWithoutProto(data)) {
                		//如果已经写完了 那么重新监听read
                		nginxs[serverfd].Addfd2Read();
               		}
               		else{
               			nginxs[serverfd].Addfd2Write();
               		}
				}
			}
		}
	}
	delete[] nginxs;
    close( pipefd );
    //close( listenfd );
    //来关闭这个文件描述符，即所谓的“对象（比如一个文件描述符，又或者一堆内存）由哪个函数创建，就应该由那个函数销毁
    close( epollfd );
}

/* 
主进程要干的事情：
1.监听一个新的端口，用于DibtNginx通信
2.连接集群中的Nginx
3.给子进程发送 accept 消息
4.给子进程发送 数据同步 消息
*/
template<class T>
void Processpool::runParent() {
	/* 创建epoll事件监听表 */
	epollfd = epoll_create(MAX_MAIN_PROCESS_EVENT);
	if(-1 == epollfd){
		LOG(ERROR) << "runParent epoll_create";
		return;
	}

	/* 事件处理Nginx fd既为角标 相当于hash表 减少了查找的时间*/
	Nginx *nginxs;
	try {
		nginxs = new Nginx[MAX_MAIN_PROCESS_EVENT + 1];
	}
	catch(const bad_alloc& e)	{
		LOG(ERROR) << "Nginx new error : " << e.what();
		return;
	}
	for(int i = 0; i < MAX_MAIN_PROCESS_EVENT + 1; ++i){
		nginxs[i].epollfd = epollfd;
	}

	/* 添加Nginxs的起始地址 */
	DtbtNginx *dbNginx = Singleton<DtbtNginx>::getInstence();
	dbNginx->nginxs = nginxs;

	//监听 listenfd 
    nginxs[dbNginx->lisSerfd].sockfd = dbNginx->lisSerfd;
    nginxs[dbNginx->lisSerfd].Addfd2Read();
    nginxs[dbNginx->lisClifd].sockfd = dbNginx->lisClifd;
    nginxs[dbNginx->lisClifd].Addfd2Read();

    /* 创建信号pipe */
	InitSigPipe(nginxs);

	/* 将子进程添加到ConsistentHash */
	for(int i = 0; i < childIdx; ++i){
		//虚拟节点20个
		dbNginx->csshash->addNode(dbNginx->nginxName + "|" + to_string(subProcess[i].pipefd[0]), 20);
		nginxs[subProcess[i].pipefd[0]].sockfd = subProcess[i].pipefd[0];
		// 监听 - 对于本程序没啥用 child dont send to father
		// [subProcess[i].pipefd[0]].Addfd2Read();
	}
	
	/* 创建Dtbt专属的 listenfd */
	int idx = dbNginx->nginxName.find(' ');
	string ipStr(dbNginx->nginxName, 0, idx);
	string portStr(dbNginx->nginxName, idx + 1, dbNginx->nginxName.size());
	int port = atoi(portStr.c_str());
	int dtbtfd = dbNginx->CreateListen(ipStr, port);
	nginxs[dtbtfd].sockfd = dtbtfd;
    nginxs[dtbtfd].Addfd2Read();

	/* 和集群中的其他节点进行连接 */
	dbNginx->ConOtherNginx();

	epoll_event events[ MAX_MAIN_PROCESS_EVENT ];
    int sub_process_counter = 0;
    int ChiMessage = 0;
    int number = 0;
    int ret = -1;
	int sockfd, connfd;
	struct sockaddr_in client_addr;
	socklen_t client_addrlen = sizeof(client_addr);
	
	/* 小根堆 用于 keepalive 和 投票倒计时 */
	dbNginx->TimeHeapAdd(2000);//心跳的间隔为2秒
	dbNginx->TimeHeapAddRaft();

	/* 一启动就会发起vote，让所有节点都知道自己的name */
	dbNginx->VoteSend();
	
	while( ! stop )	{
		number = epoll_wait( epollfd, events, MAX_MAIN_PROCESS_EVENT, dbNginx->TimeHeapGet());
		if ( ( number < 0 ) && ( errno != EINTR ) ) {
        	LOG(ERROR) << "epoll_wait < 0";
        	break;
        }
		/* timeout */
		if(0 == number){
			//如果是 跟随者 && 还没有leader 那么需要投票
			if(dbNginx->status == FOLLOWER && dbNginx->leaderName[ENSURE].empty()){
				dbNginx->status = CANDIDATE;
				dbNginx->VoteSend();
				LOG(DEBUG) << "vote for self ";
			}
			else if(dbNginx->status == CANDIDATE && dbNginx->leaderName[ENSURE].empty()){
				dbNginx->status = FOLLOWER;
				dbNginx->version[SUBMIT] = 0;//提交版本改为0 用来区别是否投票给别人了
				dbNginx->leaderName[SUBMIT].clear();
				// dbNginx->TimeHeapDel();	//如果要加的话 注意：在candidate状态变为Leader，del两次
				// dbNginx->TimeHeapAddRaft();//但是没有太大必要，选举能在极短的时间选出来
			}
		}

		/* 父进程只需要保证 DtbtNginx心跳就行了 */
		dbNginx->SendKeepAlive();

		/* deal events */
		for(int i = 0; i < number; i++)	{
			sockfd = ((Nginx *)events[i].data.ptr)->sockfd;
			/* 新的客服端到来 */
			if(sockfd == dbNginx->lisClifd) {
				//采用 RR 算法将其分配给一个子进程处理
                int i =  sub_process_counter;
                do {
                    if( subProcess[i].pid != -1 )	//按进程号次序调用
                    {
                        break;
                    }
                    i = (i + 1) % processNumber;
                } while( i != sub_process_counter );
				//如果子进程全部退出，父进程也退出
                if( subProcess[i].pid == -1 )
                {
                    stop = true;
                    break;
                }
                sub_process_counter = (i + 1) % processNumber;

				/* 给子进程发送一个消息 */
				string data("");
				if(!nginxs[subProcess[i].pipefd[0]].WriteProto(CliConNo, data)){
					nginxs[subProcess[i].pipefd[0]].Addfd2Write();
				}
                LOG(DEBUG) << "client request to child " << i;
			}
			/* 服务器连接 */
			else if(sockfd == dbNginx->lisSerfd){
				while((connfd = accept( dtbtfd, (struct sockaddr*)&client_addr, &client_addrlen)) > 0) {
					ipStr = string(inet_ntoa(client_addr.sin_addr));
					portStr = to_string(ntohs(client_addr.sin_port));
					/* 让所有进程都去连接这个server*/
					HostName hm;
					string data;
					for(int i = 0; i < processNumber; ++i){
						hm.set_ip(ipStr);
						hm.set_port(atoi(portStr.c_str()));
						hm.SerializeToString(&data);
						if(!nginxs[subProcess[i].pipefd[0]].WriteProto(SerConNo, data)){
							nginxs[subProcess[i].pipefd[0]].Addfd2Write();
						}
					}
				}
                LOG(DEBUG) << "server request to child " << i;
			}
			/* 集群 connect */
			else if(sockfd == dtbtfd){
				while((connfd = accept( dtbtfd, (struct sockaddr*)&client_addr, &client_addrlen)) > 0) {
					nginxs[connfd].AcceptNginx(connfd);
				}
			}
			/* 集群消息 */
			else if(sockfd > 0 && sockfd < MAX_MAIN_PROCESS_EVENT + 1){
				if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )  {
	                nginxs[sockfd].CloseSocket();
	            }
	            //写事件
	            else if( events[i].events & EPOLLOUT ) {
	            	if(nginxs[sockfd].sockfd == -1) {
	            		LOG(ERROR) << "write sockfd had close fd=" << sockfd;
	            	}
	                //能进入到这里面说明已经监听写了，没有写完也不用再监听写了
	                else if(nginxs[sockfd].Write()) {
	                	//如果已经写完了 那么重新监听read
	                	nginxs[sockfd].Addfd2Read();
	                }
	            }
	            //读事件
	            else if( events[i].events & EPOLLIN ){
	            	if(nginxs[sockfd].sockfd == -1){
	            		LOG(ERROR) << "Read sockfd had close fd=" << sockfd;
	            	}
					else if(nginxs[sockfd].ReadProto()){
						//数据读完 或者 close 读指针置0
						nginxs[sockfd].readIdx = 0;
					}
	            }
			}
			/* 父进程接收到的信号 */
			else if( ( sockfd == sigPipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
				char signals[10];
				if(nginxs[sockfd].Read()){
					ret = nginxs[sockfd].readIdx;
					//数据读完 或者 close 读指针置0
					nginxs[sockfd].readIdx = 0;
				}
				else{
					// 没有读完
					continue;
				}
				for( int i = 0; i < ret; ++i ) {
					// LOG(DEBUG) << "parent recv a signal:" << sigPipefd[i];
					//如果进程池中第i个子进程退出了，
					//则主进程关闭通信管道，并设置相应的pid为-1，以标记该子进程已退出
					switch( signals[i] ) {
						case SIGCHLD:
						{
							pid_t pid = -1;
							int stat;
							while( ( pid = waitpid( -1, &stat, WNOHANG) ) > 0 )	{
								/*找到是哪个子进程关闭了 并关闭父子之间的通信管道 并把pid设置-1*/
								for( int i = 0; i < processNumber; ++i ) {
									if( subProcess[i].pid == pid ) {
										LOG(INFO) << "child close " << i;
										close( subProcess[i].pipefd[0] );
										subProcess[i].pid = -1;
									}
								}
							}
							/* 如果所有子进程都已经退出了，则父进程也退出 */
							stop = true;
							for( int i = 0; i < processNumber; ++i ) {
								if( subProcess[i].pid != -1 )	{
									stop = false;//只要有一个子进程还存在，那么父进程就继续跑
								}
							}
							break;
						}
						case SIGTERM:
						case SIGINT:
						{
							//如果父进程接收到终止信号，那么就杀死所有子进程，并等待它们全部结束，当然，
							//通知子进程结束更好的方法是向父/子进程之间的通信管道发送特殊数据
							LOG(INFO) << "kill all the clild now";
							for( int i = 0; i < processNumber; ++i ) {
								int pid = subProcess[i].pid;
								if( pid != -1 )	{
									kill( pid, SIGTERM );
									// kill( pid , SIGKILL );
								}
								else {
									LOG(ERROR) << "pid = " << pid;
								}
							}
							break;
						}
						default:break;
					}
				}
			}
			else{
				LOG(ERROR) << "epoll socket = " << sockfd;
			}
		}
	}
	delete[] nginxs;
    close( listenfd );
    close( epollfd );
}
#endif
/*
int sigfillset(sigset_t * set);
sigfillset()用来将参数set信号集初始化，然后把所有的信号加入到此信号集里即将所有的信号标志位置为1
成功时则返回0；如果有错误则返回-1，并设置errno的值，如果errno的值为EFAULT，则表示参数set指针地址无法存取。

int socketpair(int d, int type, int protocol, int sv[2]);
可以用于网络通信，也可以用于本机内的进程通信
四个参数:		套接口的域　　套接口类型　　使用的协议　　指向存储文件描述符的指针
对于socketpair函数，protocol参数必须提供为0
RETURN VALUE
       On success, zero is returned.  On error, -1 is returned, and errno is set appropriately.
	   
signal(SIGCHLD, SIG_IGN); 
忽略SIGCHLD信号，这常用于并发服务器的性能的一个技巧
因为并发服务器常常fork很多子进程，子进程终结之后需要服务器进程去wait清理资源。
如果将此信号的处理方式设为忽略，可让内核把僵尸子进程转交给init进程去处理，省去了大量僵尸进程占用系统资源。

*/
/*
1.SIGINT SIGTERM 区别
前者与字符ctrl+c关联，后者没有任何控制字符关联。
前者只能结束前台进程，后者则不是。
2.SIGTERM SIGKILL的区别
前者可以被阻塞、处理和忽略，但是后者不可以。KILL命令的默认不带参数发送的信号就是SIGTERM.让程序有好的退出。
因为它可以被阻塞，所以有的进程不能被结束时，用kill发送后者信号，即可。即：kill -9 进程号。

调度方法
1，SCHED_OTHER 分时调度策略，
2，SCHED_FIFO实时调度策略，先到先服务
3，SCHED_RR实时调度策略，时间片轮转


总结:
这个错误表示资源暂时不够，能read时，读缓冲区没有数据，或者write时，写缓冲区满了。
遇到这种情况，如果是阻塞socket，read/write就要阻塞掉。而如果是非阻塞socket，read/write立即返回-1， 同时errno设置为EAGAIN。
所以，对于阻塞socket，read/write返回-1代表网络出错了。但对于非阻塞socket，read/write返回-1不一定网络真的出错了。
可能是Resource temporarily unavailable。这时你应该再试，直到Resource available。
*/
