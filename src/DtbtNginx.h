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
#include "Singleton.h"
#include "ReadConf.h"
// #include "../consHash/ConsistentHash.h"
using namespace std;

enum STATU{FOLLOWER, CANDIDATE, LEADER};//追随者，候选者，领导者
enum {ENSURE, SUBMIT};
/*
使用进程池的心得：
铁了心想用进程池，但是有很多数据需要共享，没办法必须mmap数据到共享区(再仔细想就算不共享也没啥，还节省了锁的开销)
所以必须每个子进程都得有一个 consistent hash object 我想了很久，这样其实也没有什么不妥的
当后台只有4台服务器的时候，我进程池就创建一个子进程，redis也是一个单进程服务器
但是redis 采用多路 I/O 复用技术照样可以让单个线程高效的处理多个连接请求
redis的缺点：不能充分发挥多核的优势
对于redis的这个缺点我进行修改：子进程的数量 = 当后台服务器 / N

使用进程池的好处：
1.程序的健壮性很强，一个进程挂了，我再创建一个新的就好，不会把整个程序给拖垮
2.通过增加CPU，就可以容易扩充性能
3.可以尽量减少线程加锁/解锁的影响
4.从资源的占有量上来说比线程池大很多
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

	/* client and server communicate data */
	vector<string> backServers[2];//后台活着的服务器ip port(属于需要同步的数据) [0]已经确认[1]提交单位确认
	map<int, int> keepSession[2];//client和server之间的会话保持 [0]已经确认[1]提交单位确认
	ConsistentHash *csshash;//一致性hash客户端，只有leader需要，由于用了多进程所以每个进程都会有一个
	vector<int> timeHeap;//时间管理-小根堆
	Nginx *nginxs;//因为需要给其他节点发送消息，所以得保存起始地址
	map<string, int> mSerNamefd;//记录服务器的name->fd
	map<int, string> mSerfdName;//活着的server fd
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
	/* 主动发送proto的函数 */
	void VoteSend();//向所有节点发送投票
	void AckVote2FollowerSend();//向所有节点发送自己是Leader
	void SynchDataSend();//向所有节点发送数据同步
	void AckData2FollowerSend();//向所有节点发送数据同步ACK

	void SendKeepAlive();//检查心跳是否超时 并且发送心跳

	void TimeHeapAdd(size_t timeout);
	void TimeHeapDel();
	int TimeHeapGet();
	void TimeHeapAddRaft();
};
#endif