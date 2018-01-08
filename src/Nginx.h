#ifndef NGINX_H
#define NGINX_H
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
#include <map>
#include <string>

using namespace std;

class DtbtNginx;
/* 每个客户端都分配一个Nginx */
class Nginx
{
public:
	/* 全局的DtbtNginx指针 */
	DtbtNginx *dbNginx;
	int epollfd;//因为有些数据可能写不完 有些数据发不完 需要继续监听 所以需要这个变量
	/* 关于epoll的数据 */
	int events;//epoll的模式 et lt 等
	int eStatus;//0:还未加入epoll 1:已经加入epoll----避免重复添加到epoll
	int keepAliveInterval;//心跳的间隔时间 秒
	int activeInterval;//断开时间
	long lastKeepAlive;//上次心跳的时间(秒) 从1970年1月1日开始
	long lastActive;//上次活跃的时间(秒) 从1970年1月1日开始
	/* 内部的数据 */
	char *readBuf;//读到的数据
	size_t readIdx;//读到的位置（可能一次读不全）
	size_t readSize;//这次读到的大小-readIdx返回就置为0了
	int readBufSize;//buf大小
	char *writeBuf;//写的数据--避免没有写完
	int writeIdx;
	int writeLen;
	int writeBufSize;
	string clientName;//ip prot
	int sockfd;//发送或者接收的套接字
	typedef void(Nginx::*func)(char *proto);
	map<int, func> callBack;//回调函数

	/* parse http */
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATCH };//各种请求
	enum HTTP_CODE { NO_REQUEST, //请求不完整，需要继续读客户数据
					GET_REQUEST, //得到一个完整的客户请求
					BAD_REQUEST, //请求语法错误
					NO_RESOURCE, //没有资源
					FORBIDDEN_REQUEST, //客户端对资源没有足够的访问权限
					FILE_REQUEST, //文件请求
					INTERNAL_ERROR, //服务器内部错误
					CLOSED_CONNECTION /*客户端关闭连接*/};
	enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };//读取状态：1.读到完整行 2.行出错 3.数据不完整
	enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };//正在分析请求行、头部字段、内容
	/*文件名最大长度*/
	static const int FILENAME_LEN;
	/* request 最大长度 */
	static const int MAXHTTPREQUEST;//占时设置4k
	static const char* httpFileRoot;//到时候可以写入配置文件----
	/* 响应码 */
	static const char* ok_200_title;
	static const char* error_301_title;
	static const char* error_400_title;
	static const char* error_400_form;
	static const char* error_403_title;
	static const char* error_403_form;
	static const char* error_404_title;
	static const char* error_404_form;
	static const char* error_500_title;
	static const char* error_500_form;
    /* 解析到的位置 */
    size_t checkedIdx;
    /* 正在解析的行的起始位置 */
    size_t startLine;
    /* 主状态机当前所处的状态 */
    CHECK_STATE checkState;
    /* 解析方法 */
    METHOD httpMethod;
	/* 文件名-绝对路径 */
    string fileName;
	/* http协议号 */
    char *httpVer;
	/* 消息体长度 */
    size_t contentLength;
	/* http是否保持连接 */
    bool keepLinger;
    /* http host name */
    string httpHost;
    /* 目标文件状态，是否是目录，是否可读，文件大小 */
	struct stat fileStat;
	/* 解析response的时候 http头的大小*/
	size_t httpHeaderSize;
public:
	Nginx(size_t readBufSize = 2048, size_t writeBufSize = 2048);
	~Nginx();
	bool Read();
	bool ReadHttpRequest();
	bool ReadHttpResponse();
	bool ReadProto();
	bool Write();
	bool WriteWithoutProto(string &data);
	bool WriteProto(int cmd, string &data);
	void ExpandBuf(char *&srcBuf, size_t srcSize, size_t distSize);
	/* 收到proto的回调函数 */
	void VoteRcve(char *proto);
	void AckVote2LeaderRcve(char *proto);//响应投票1
	void AckVote2FollowerRcve(char *proto);//响应投票2
	void SynchDataRcve(char *proto);//同步数据
	void AckData2LeaderRcve(char *proto);//响应同步数据
	void AckData2FollowerRcve(char *proto);//再响应同步数据
	void KeepAliveRcve(char *proto);
	/* 收到父进程的回调函数 */
	void SerCon(char *proto);
	void CliCon(char *proto);
	/* 主动发送proto的函数 */
	void AckVote2FollowerSend();//投票响应 只发给一个节点

	/* 解析http request */
	HTTP_CODE ParseRequest();
	HTTP_CODE ParseRequestLine(char *text);
	HTTP_CODE ParseRequestHeader(char *text);
	HTTP_CODE ParseRequestContent(char *text);
	LINE_STATUS ParseBlankLine();
	/* do request */
	HTTP_CODE DoRequest();
	/* response */
	void CacheResponseHeader(HTTP_CODE ret = INTERNAL_ERROR);
	bool WriteHttpHeader(HTTP_CODE ret);
	bool AddResponse( const char* format, ... );
	bool AddStatusLine( int status, const char* title );
	bool AddHeaders( int fileLen, const char *location = NULL);
	bool AddContentLength( int fileLen );
    bool AddContent( const char* content );
    bool AddLinger();
	bool AddLocation(const char *otherUrl);
    bool AddBlankLine();

    /* 解析服务器发过来的response */
    bool ParseResponse();
	/* 把消息转发给服务器 */
	void Response2Server(char *buf, int size, bool keepLinger);

	/* 关于epoll */
	int SetNoBlocking(int fd);
	void Addfd2Read();
	void Addfd2Write();
	void Removefd();
	void SetTimeout(int keepAliveInterval, int activeInterval);

	bool CheckServerClose();
	bool CheckNginxClose();
	void CloseSocket();
	void CloseServer();
	void CloseNginx();
	void ClearClient();
	void ClearSocket();
	void ClearResponse();

	void AcceptNginx(int sockfd);
	void AcceptServer(int sockfd, string &name);
	void AcceptClient(int sockfd, string &name);
};
#endif