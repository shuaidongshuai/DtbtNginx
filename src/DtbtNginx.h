#ifndef DTBTNGINX_H
#define DTBTNGINX_H
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
#include <map>
#include <string>
#include <vector>
#include <list>
#include "Singleton.h"
#include "ReadConf.h"
using namespace std;

enum STATU{FOLLOWER, CANDIDATE, LEADER};//追随者，候选者，领导者
enum {ENSURE, SUBMIT};
enum {WEB, LOAD};
/*
使用进程池的心得：
铁了心想用进程池，但是有很多数据需要共享，没办法必须mmap数据到共享区(再仔细想就算不共享也没啥，还节省了锁的开销)
所以必须每个子进程都得有一个 consistent hash object 我想了很久，这样其实也没有什么不妥的
当后台只有4台服务器的时候，我进程池就创建一个子进程，redis也是一个单进程服务器
但是redis 采用多路 I/O 复用技术照样可以让单个线程高效的处理多个连接请求
redis的缺点：不能充分发挥多核的优势
对于redis的这个缺点我进行修改：子进程的数量 = 当后台服务器 / N
*/
/* 
安全问题：
1.服务器过来连接我没有做安全检查--其实可以用https那样的 密码-秘钥-数字签名
 */
class Nginx;
class ConsistentHash;

class DtbtNginx
{
public:
	/* DtbtNginx cluster communicate data  */
	enum STATU status;//自己是哪个状态
	int voteNum;//投票的数量
	int allNginxNum;//配置文件中所有Nginx数量
	/* Dtbt名是内部cluster使用名称 listen名 client and server 使用的--格式：ip port */
	vector<string> otherName;//配置文件中除自己之外集群的Dtbt名
	vector<int> aliveNginxfd;//活着的nginx fd  --connect、accept的时候加
	string nginxName;//自己Dtbt名
	string lisSerName;//自己lisSer名
	string lisCliName;//自己lisCli名
	string leaderName[2];//ip port [0]为空说明Leader还未产生 [1]是提交但未确认
	int lisSerfd;
	int lisClifd;
	int version[2];//[0]已经确认的版本 [1]提交但未确认的版本
	int nginxMode;//两种模式 0.web服务器 1.负载均衡  将来可能有更多模式

	/* client and server communicate data */
	map<int, int> keepSession[2];//client和server之间的会话保持 [0]已经确认[1]提交单位确认
	list<pair<int, int>> sSer2Cli;//这个是为了处理server->client  (serfd, clifd)
	ConsistentHash *csshash;//一致性hash客户端，只有leader需要，由于用了多进程所以每个进程都会有一个
	vector<int> timeHeap;//时间管理-小根堆
	Nginx *nginxs;//因为需要给其他节点发送消息，所以得保存起始地址
	vector<string> backServers;//后台所有的服务器ip port
	map<string, int> mSerNamefd;//记录服务器的name->fd
	map<int, string> mSerfdName;//记录服务器的fd->name

	/* const data */
	const size_t raftVoteTime = 150;
protected:
	DtbtNginx();
	~DtbtNginx();
	friend Singleton<DtbtNginx>;
public:
	/* 加载配置文件 */
	bool ReadDtbtNginxConf(string hostName, string confSrc);
	/* 监听端口 */
	int CreateListen(string ip, int port);
	/* 启动的时候和集群中的其他节点进行连接 */
	void ConOtherNginx();
	/* 连接后台服务器 */
	void ConServer();
	/* 主动发送proto的函数 */
	void VoteSend();//向所有节点发送投票
	void AckVote2FollowerSend();//向所有节点发送自己是Leader
	void SynchDataSend();//向所有节点发送数据同步
	void AckData2FollowerSend();//向所有节点发送数据同步ACK

	/* 检查心跳是否超时 并且发送心跳 */
	bool checkLastActive(int fd, int curTime);
	void checkKeepAlive(int fd, int curTime);
	void SendKeepAlive2Nginx();
	void SendKeepAlive2SC();

	void TimeHeapAdd(size_t timeout);
	void TimeHeapDel();
	int TimeHeapGet();
	void TimeHeapAddRaft();

	int FindClifdBySerfd(int sockfd);
};
#endif