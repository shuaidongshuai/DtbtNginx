#include "Nginx.h"
#include "DtbtNginx.h"
#include "protoCallBack.h"
#include "inNginx.pb.h"
#include "easylogging++.h"
#include "Singleton.h"
#include <ctime>
#include "ConsistentHash.h"

/* 默认et模式 */
Nginx::Nginx(size_t readBufSize, size_t writeBufSize)
			:events(EPOLLET), eStatus(0), readBufSize(readBufSize), readIdx(0),
			writeIdx(0),writeLen(0),writeBufSize(writeBufSize),
			sockfd(-1), keepAliveInterval(-1), activeInterval(-1){//-1代表无限制
	dbNginx = Singleton<DtbtNginx>::getInstence();
	readBuf = new char[readBufSize];
	writeBuf = new char[writeBufSize];
	/* 注册回调函数 */
	callBack[VoteNo] = &Nginx::VoteRcve;
	callBack[AckVote2LeaderNo] = &Nginx::AckVote2LeaderRcve;
	callBack[AckVote2FollowerNo] = &Nginx::AckVote2FollowerRcve;

	callBack[SynchDataNo] = &Nginx::SynchDataRcve;
	callBack[AckData2LeaderNo] = &Nginx::AckData2LeaderRcve;
	callBack[AckData2FollowerNo] = &Nginx::AckData2FollowerRcve;

	callBack[KeepAliveNo] = &Nginx::KeepAliveRcve;

	callBack[SerConNo] = &Nginx::SerCon;
	callBack[CliConNo] = &Nginx::CliCon;

	callBack[Server2NginxNo] = &Nginx::Server2NginxRcve;
}

Nginx::~Nginx(){
	delete readBuf;
}

void Nginx::SetReadBuf(int size){
	char *newBuf = new char[size];
	char *temp = readBuf;
	readBuf = newBuf;
	readBufSize = size;
	delete temp;
}
/* 普通的读 */
bool Nginx::Read(){
	lastActive = time(0);//记录活跃的时间
	int nread = 0;
	while(readIdx < 2 * sizeof(int)){
		nread = read(sockfd, readBuf + readIdx, 2 * sizeof(int) - readIdx);
		if (nread == -1) {
			if(errno == EAGAIN){
				return false;
			}
			LOG(INFO) << "客服端异常退出";
			CloseSocket();
			return true;
		}
		if(nread == 0) {
			LOG(INFO) << "客服端正常退出";
			CloseSocket();
			return true;
		}
		readIdx += nread;
	}
	return true;
}
/*
所有数据都是 cmd+len+protobuf  要按照这个来读 cmd 和 len 都是 int
返回false说明没有读完，接着读。true说明不需要继续读了 
*/
bool Nginx::ReadProto(){
	lastActive = time(0);//记录活跃的时间
	int count = 5;//不能被一个恶意的用户给拖垮了，最多读count次
	while(count--){
		int nread = 0;//1.一次读到的size 2.一共读到的大小 3.最多读10次(避免网络丢包)
		//读 cmd 和 len 只有小于8Byte才去读它
		while(readIdx < 2 * sizeof(int)){
			nread = read(sockfd, readBuf + readIdx, 2 * sizeof(int) - readIdx);
			if (nread == -1) {
				if(errno == EAGAIN){
					return false;
				}
				LOG(INFO) << "客服端异常退出";
				CloseSocket();
				return true;
			}
			if(nread == 0) {
				LOG(INFO) << "客服端正常退出";
				CloseSocket();
				return true;
			}
			readIdx += nread;
		}
		//读protobuf
		int cmd = *(int *)readBuf;
		int len = *((int *)readBuf + 1);
		if(len > 1024 * 1024){
			LOG(INFO) << "len to long :" << len;
			CloseSocket();
			return true;
		}
		if(2 * sizeof(int) + len > readBufSize){
			delete readBuf;
			readBufSize = 2 * sizeof(int) + len;
			readBuf = new char[readBufSize];
		}
		while(readIdx < 2 * sizeof(int) + len){
			nread = read(sockfd, readBuf + readIdx, 2 * sizeof(int) + len - readIdx);
			if (nread == -1) {
				if(errno == EAGAIN)
					return false;
				LOG(INFO) << "客服端异常退出";
				CloseSocket();
				return true;
			}
			if(nread == 0) {
				LOG(INFO) << "客服端正常退出";
				CloseSocket();
				return true;
			}
			readIdx += nread;
		}
		LOG(DEBUG) << "读到" << clientName << "：" << readBuf << "---" << readIdx << "字节";
		//执行回调函数
		(this->*(callBack[cmd]))(readBuf + 2 * sizeof(int));
		//数据读完 读指针置0
		readIdx = 0;
	}
	return true;
}
/* 还需要监听就返回false */
bool Nginx::Write(){
	int n;
	while (1){
		n = write(sockfd, writeBuf + writeIdx, writeLen - writeIdx);
		if ( n <= -1 ) {
			if( errno == EAGAIN ) {
				LOG(DEBUG) << "当前不可写，继续监听写事件";
				return false;
			}
			LOG(INFO) << "发送失败";
			return true;
		}
		writeIdx += n;
		if(writeIdx >= writeLen){
			// 写完了要监听读事件
			Addfd2Read();
			break;
		}
	}
	//LOG(DEBUG) << "写:" << writeBuf << "-" << writeLen << "字节";
	return true;
}

bool Nginx::WriteWithoutProto(string &data){
	writeLen = data.size();
	writeIdx = 0;
	if(writeLen > writeBufSize){
		delete writeBuf;
		writeBuf = new char[writeLen];
		writeBufSize = writeLen;
	}
	strncpy(writeBuf, data.c_str(), writeLen);
	return Write();
}
/* 发送protobuf协议 加上包头(cmd + len) */
bool Nginx::WriteProto(int cmd, string &data){
	int len = data.size();
	writeLen = 2 * sizeof(int) + len;//注意要修改writeLen
	writeIdx = 0;//注意要修改writeIdx
	if(writeLen > writeBufSize){
		delete writeBuf;
		writeBuf = new char[writeLen];
		writeBufSize = writeLen;
	}
	strncpy(writeBuf, (char *)&cmd, sizeof(int));
	strncpy(writeBuf + sizeof(int), (char *)&len, sizeof(int));
	strncpy(writeBuf + 2 * sizeof(int), data.c_str(), len);
	return Write();
}
//收到投票--Leader-Candiate-Follower
void Nginx::VoteRcve(char *proto){
	if(!proto){
		LOG(ERROR) << "VoteRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
	Vote vote;
	bool parseRes = vote.ParseFromString(data);
	if(!parseRes){
		LOG(ERROR) << "VoteRcve parseRes is false";
		return;
	}
	int ver = vote.version();
	string nginxName = vote.nginxname();
	/* 
	因为他连接过来的ip:port并不是我们期望的name
	所以在accept的时候不需要保存他的ip：port，在他发起投票才修改name
	每个节点起码都会发起一次投票(一启动就会发起vote)
	*/
	clientName = nginxName;
	//判断自己的状态只有follower才能为别人投票 && 新版本号比自己老版本大 && 还没有投票给别人
	if(dbNginx->status == FOLLOWER && ver > dbNginx->version[ENSURE] && 
				dbNginx->leaderName[ENSURE].empty() && !dbNginx->version[SUBMIT]){
		dbNginx->version[SUBMIT] = ver;
		dbNginx->leaderName[SUBMIT] = nginxName;
		//发ack
		string data;
		AckVote2Leader av2l;
		av2l.set_version(ver);
		av2l.set_nginxname(nginxName);
		av2l.SerializeToString(&data);
		if( !WriteProto(AckVote2LeaderNo, data) ){
			Addfd2Write();
		}
		else{
			Addfd2Read();
		}
		LOG(DEBUG) << "vote for " << nginxName;
	}
	//如果自己是Leader那么需要告诉他
	else if(dbNginx->status == LEADER){
		//如果已经有Leader了，那么告诉他
		AckVote2FollowerSend();
		LOG(INFO) << nginxName << " is survived";
	}
	else{
		LOG(INFO) << nginxName << "not vote for " << nginxName;
	}
}
//收到投票响应1
void Nginx::AckVote2LeaderRcve(char *proto){
	LOG(DEBUG) << "recv a vote for me";
	if(!proto){
		LOG(ERROR) << "AckVote2LeaderRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
	AckVote2Leader av2l;
	bool parseRes = av2l.ParseFromString(data);
	if(!parseRes){
		LOG(ERROR) << "AckVote2LeaderRcve parseRes is false";
		return;
	}
	int ver = av2l.version();
	string nginxName = av2l.nginxname();
	if(ver != dbNginx->version[ENSURE] + 1 || nginxName != dbNginx->nginxName){
		LOG(ERROR) << "VoteAckRcve ver:" << ver << " nginxName:" << nginxName;
		return;
	}
	//如果票数大于一半  自己变为Leader
	if(++dbNginx->voteNum > dbNginx->allNginxNum / 2){
		//改变状态
		dbNginx->status = LEADER;
		++dbNginx->version[ENSURE];
		dbNginx->leaderName[ENSURE] = dbNginx->nginxName;
		//时间堆里面去除投票超时时间-注：投票超时时间是最小的时间
		dbNginx->TimeHeapDel();
		//给所有节点发送自己是Leader信息
		dbNginx->AckVote2FollowerSend();
		LOG(INFO) << "become a Leader";
	}
	else{
		LOG(DEBUG) << "all vete num = " << dbNginx->voteNum;
	}
}
//收到投票响应2
void Nginx::AckVote2FollowerRcve(char *proto){
	LOG(DEBUG) << "AckVote2FollowerRcve";
	if(!proto){
		LOG(ERROR) << "AckVote2FollowerRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
	AckVote2Follower av2f;
	bool parseRes = av2f.ParseFromString(data);
	if(!parseRes){
		LOG(ERROR) << "AckVote2FollowerRcve parseRes is false";
		return;
	}
	int ver = av2f.version();
	string nginxName = av2f.nginxname();
	if(ver >= dbNginx->version[SUBMIT]){
		//说明Leader已经选出来了
		dbNginx->version[ENSURE] = ver;
		dbNginx->leaderName[ENSURE] = nginxName;
		dbNginx->status = FOLLOWER;
		dbNginx->TimeHeapDel();
		LOG(INFO) << nginxName << " become a Leader, I am a follower";
	}
}
/* 收到同步数据 */
void Nginx::SynchDataRcve(char *proto){
	if(!proto){
		LOG(ERROR) << "SynchDataRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
	AckData2Leader ad2l;
	bool parseRes = ad2l.ParseFromString(data);
	if(!parseRes){
		LOG(ERROR) << "SynchDataRcve parseRes is false";
		return;
	}
	//...
}
/* 响应同步数据1 */
void Nginx::AckData2LeaderRcve(char *proto){
	if(!proto){
		LOG(ERROR) << "AckData2LeaderRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
	AckData2Leader ad2l;
	bool parseRes = ad2l.ParseFromString(data);
	if(!parseRes){
		LOG(ERROR) << "AckData2LeaderRcve parseRes is false";
		return;
	}
	//...
}
/* 响应同步数据2 */
void Nginx::AckData2FollowerRcve(char *proto){
	if(!proto){
		LOG(ERROR) << "AckData2FollowerRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
	//...
}
/* 心跳 */
void Nginx::KeepAliveRcve(char *proto){
	if(!proto){
		LOG(ERROR) << "KeepAliveRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
}

void Nginx::AckVote2FollowerSend() {
	string data;
	AckVote2Follower avf;
	avf.set_version(dbNginx->version[ENSURE]);//版本已经升级过了
	avf.set_nginxname(dbNginx->leaderName[ENSURE]);
	avf.SerializeToString(&data);
	if(!WriteProto(AckVote2FollowerNo, data)){
		Addfd2Write();
	}
	else{
		Addfd2Read();
	}
}
//连接服务器
void Nginx::SerCon(char *proto){
	if(!proto){
		LOG(ERROR) << "SerCon nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "SerCon len < 0";
		return;
	}
	string data(proto, len);
	HostName hm;
	bool parseRes = hm.ParseFromString(data);
	if(!parseRes){
		LOG(ERROR) << "SerCon parseRes is false";
		return;
	}
	string ip = hm.ip();
	int port = hm.port();
	//连接
	int sockfd;
	if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		LOG(ERROR) << "ConOtherNginx create socket error";
		return;
	}
	struct sockaddr_in servaddr;
	bzero(&servaddr,sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr);
	servaddr.sin_port = htons(port);
	if(connect(sockfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) == -1) {
		LOG(WARNING) << "Not Connected:" << ip << ":" << port;
		//这里应该把消息报告写给父进程 waiting for you to complete
		//...
	}
	else{
		string name(ip + " " + to_string(port));
		AcceptServer(sockfd, name);
		LOG(DEBUG) << "Connected Server:" << ip << ":" << port;
	}
}
//连接客户端
void Nginx::CliCon(char *proto){
	if(!proto){
		LOG(ERROR) << "SerCon nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "SerCon len < 0";
		return;
	}
	while((connfd = accept( dbNginx->lisClifd, (struct sockaddr*)&client_addr, &client_addrlen)) > 0) {
		ipStr = string(inet_ntoa(client_addr.sin_addr));
		portStr = to_string(ntohs(client_addr.sin_port));
		//name
		clientName = ipStr + " " + portStr;
		//记录fd
		sockfd = connfd;
		//设置超时时间
		SetTimeout(2, 6);//心跳2s、断开6s
		//监听读事件
		Addfd2Read();
	}
    LOG(DEBUG) << "server request to child " << i;
}

void Nginx::Server2NginxRcve(char *proto){
	if(!proto){
		LOG(ERROR) << "AckVote2LeaderRcve nullptr";
		return;
	}
	int len = readIdx - 2 * sizeof(int);
	if(len < 0){
		LOG(ERROR) << "VoteRcve len < 0";
		return;
	}
	string data(proto, len);
	Server2Nginx s2n;
	bool parseRes = s2n.ParseFromString(data);
	if(!parseRes){
		LOG(ERROR) << "AckVote2LeaderRcve parseRes is false";
		return;
	}
	int ver = s2n.port();
	string nginxName = s2n.text();
	



}

int Nginx::SetNoBlocking(int fd) {
	int old_option = fcntl(fd,F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	return fcntl(fd, F_SETFL, new_option);//如果出错，所有命令都返回-1
}

void Nginx::Addfd2Read() {
	if(SetNoBlocking(sockfd) < 0) {
		LOG(WARNING) << "SetNoBlocking 失败 sockfd = " << sockfd;
		return;
	}
	epoll_event event;
	int op;
	//如果已经在epoll里了，把加 变成 修改
	if(!eStatus) {
		op = EPOLL_CTL_ADD;
		eStatus = 1;
	}
	else {
		op = EPOLL_CTL_MOD;
	}
	/*
	注意：
	event.data 是一个 union 也就是说 fd 和 void *只能设置一个
	这个bug找了很久
	*/
	// event.data.fd = sockfd;
	event.data.ptr = this;
	event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;// | EPOLLONESHOT
	/*
	EPOLLONESHOT：只监听一次事件，当监听完这次事件之后,如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里 
	由于我是用的是 类似单进程模式 所以不需要这个参数
	EPOLLRDHUP: 当使用这个参数后 子进程close(退出)不会通知父进程
	*/ 
	epoll_ctl(epollfd, op, sockfd, &event);
}

void Nginx::Addfd2Write() {
	if(SetNoBlocking(sockfd) < 0) {
		LOG(WARNING) << "SetNoBlocking 失败 sockfd = " << sockfd;
		return;
	}
	epoll_event event;
	int op;
	if(!eStatus) {
		op = EPOLL_CTL_ADD;
		eStatus = 1;
	}
	else {
		op = EPOLL_CTL_MOD;
	}
	//event.data.fd = sockfd;
	event.data.ptr = this;
	event.events = EPOLLOUT | EPOLLET | EPOLLRDHUP;// | EPOLLONESHOT 
	epoll_ctl(epollfd, op, sockfd, &event);
}

void Nginx::Removefd() {
	epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
	sockfd = -1;
	eStatus = 0;
	readIdx = 0;
	CloseSocket();
}
//设置 心跳、断开 相对时间(s)
void Nginx::SetTimeout(int keepAliveInterval, int activeInterval){
	this->lastActive = time(0);
	this->lastKeepAlive = this->lastActive;
	this->keepAliveInterval = keepAliveInterval;
	this->activeInterval = activeInterval;
}

void Nginx::CloseSocket(){
	close(sockfd);
	sockfd = -1;
	eStatus = 0;
	lastActive = 0;
	lastKeepAlive = 0;
	readIdx = 0;
	writeIdx = 0;
	writeLen = 0;
}

void Nginx::AcceptNginx(int sockfd){
	/* 除了启动的时候加入对方fd  连接的时候也要加 */
	//记录集群存活的节点fd
	dbNginx->aliveNginxfd.push_back(sockfd);
	//记录fd
	sockfd = sockfd;
	//设置超时时间
	SetTimeout(2, 6);//心跳2s、断开6s
	//监听读事件
	Addfd2Read();

	LOG(DEBUG) << "AcceptNginx:" << sockfd;
}

void Nginx::AcceptServer(int sockfd, string &name){
	//记录name
	clientName = name;
	//记录fd
	dbNginx->nginxs[sockfd].sockfd = sockfd;
	//设置超时时间
	dbNginx->nginxs[sockfd].SetTimeout(2, 6);//心跳2s、断开6s
	//监听读事件
	dbNginx->nginxs[sockfd].Addfd2Read();
	//加入map
	dbNginx->mSerNamefd.insert(make_pair(clientName, sockfd));
	dbNginx->mSerfdName.insert(make_pair(sockfd, clientName));

	LOG(DEBUG) << "AcceptServer:" << sockfd;
}

void Nginx::AcceptClient(int sockfd, string &name){
	//记录name
	clientName = name;
	//记录fd
	dbNginx->nginxs[sockfd].sockfd = sockfd;
	//设置超时时间
	dbNginx->nginxs[sockfd].SetTimeout(2, 6);//心跳2s、断开6s
	//监听读事件
	dbNginx->nginxs[sockfd].Addfd2Read();
	LOG(DEBUG) << "AcceptClient:" << sockfd;
}