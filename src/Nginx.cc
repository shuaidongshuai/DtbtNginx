#include "Nginx.h"
#include "DtbtNginx.h"
#include "protoCallBack.h"
#include "inNginx.pb.h"
#include "easylogging++.h"
#include "Singleton.h"
#include "ConsistentHash.h"
#include <sys/sendfile.h>
#include <ctime>

const int Nginx::FILENAME_LEN = 200;
const int Nginx::MAXHTTPREQUEST = 4 * 1024;//占时设置4k
const char* Nginx::httpFileRoot = "html/";//到时候可以写入配置文件----
const char* Nginx::ok_200_title = "OK";
const char* Nginx::error_301_title = "Found Other";
const char* Nginx::error_400_title = "Bad Request";
const char* Nginx::error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* Nginx::error_403_title = "Forbidden";
const char* Nginx::error_403_form = "You do not have permission to get file from this server.\n";
const char* Nginx::error_404_title = "Not Found";
const char* Nginx::error_404_form = "The requested file was not found on this server.\n";
const char* Nginx::error_500_title = "Internal Error";
const char* Nginx::error_500_form = "There was an unusual problem serving the requested file.\n";

/* 默认et模式 */
Nginx::Nginx(size_t readBufSize, size_t writeBufSize)
			:events(EPOLLET), eStatus(0), readBufSize(readBufSize), readIdx(0),
			writeIdx(0),writeLen(0),writeBufSize(writeBufSize), sockfd(-1), //-1代表无限制
			keepAliveInterval(-1), activeInterval(-1), checkedIdx(0), 
			startLine(0), checkState(CHECK_STATE_REQUESTLINE), httpMethod(GET), httpVer(0), contentLength(0),
			keepLinger(false)
			{
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

	// callBack[Server2NginxNo] = &Nginx::Server2NginxRcve;
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
/* 返回true都是异常 */
bool Nginx::Read(){
	
}
/* 读http请求 true代表解析完一个请求 */
bool Nginx::ReadHttp(){
	lastActive = time(0);//记录活跃的时间
	int nread = 0;
	int maxRead = MAXHTTPREQUEST;
	while(readIdx < readBufSize){
		nread = read(sockfd, readBuf + readIdx, readBufSize - readIdx);
		if (nread == -1) {
			if(errno == EAGAIN){
				return false;
			}
			LOG(INFO) << "客服端异常退出";
			CloseSocket();
			return true;
		}
		if(nread == 0) {
			// LOG(INFO) << "客服端正常退出";
			CloseSocket();
			return true;
		}
		readIdx += nread;
		/* 读一次就解析一次 */
		HTTP_CODE read_ret = ParseRequest();
	    if ( read_ret != NO_REQUEST ) {
	    	/* 缓存响应头部 */
   			bool write_ret = WriteHttpHeader( read_ret );
			if ( ! write_ret ) {
				LOG(WARNING) << "缓存响应头部 false";
				CloseSocket();
			}
			else{
				// LOG(DEBUG) << "parse success";
				ClearResponse();
			}
	    	return true;
	    }
	}
	if(readIdx > readBufSize){
		LOG(WARNING) << "request len > readBufSize";
		CloseSocket();
	}
	return true;
}
/* 解析http request */
Nginx::HTTP_CODE Nginx::ParseRequest(){
	LINE_STATUS line_status = LINE_OK;//读取状态
    HTTP_CODE ret = NO_REQUEST;		//占时请求状态设置为不完整
    char* text = 0;

	/*下面就是限状态机编程方法*/
    while ( ( ( checkState == CHECK_STATE_CONTENT ) && ( line_status == LINE_OK  ) )//主状态机：正在分析请求内容。从状态机：读到完整行
                || ( ( line_status = ParseBlankLine() ) == LINE_OK ) )//从状态机读到\r\n
    /* 要么读到\r\n 要么状态是请求体 */
    {
        text = readBuf + startLine;
        startLine = checkedIdx;

        switch ( checkState ) {
			/*解析请求消息行*/
            case CHECK_STATE_REQUESTLINE:
            {
            	// LOG(DEBUG) << "解析请求消息行";
                ret = ParseRequestLine( text );
                if ( ret == BAD_REQUEST ) {
                	LOG(DEBUG) << "RequestLine bad ";
                    return BAD_REQUEST;
                }
                break;
            }
			/*解析请求消息头*/
            case CHECK_STATE_HEADER:
            {
            	// LOG(DEBUG) << "解析请求消息头";
                ret = ParseRequestHeader( text );
                if ( ret == BAD_REQUEST ) {
                	LOG(DEBUG) << "RequestHeader bad ";
                    return BAD_REQUEST;
                }
                else if ( ret == GET_REQUEST ) {
                    return DoRequest();
                }
                break;
            }
			/*解析请求消息体*/
            case CHECK_STATE_CONTENT:
            {
            	// LOG(DEBUG) << "解析请求消息体";
                ret = ParseRequestContent( text );
                if ( ret == GET_REQUEST ) {
                    return DoRequest();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}
/*解析请求消息行*/
Nginx::HTTP_CODE Nginx::ParseRequestLine(char *text) {
	char *temp = strpbrk( text, " \t" );
    if ( ! temp ) {
    	LOG(INFO) << "RequestLine error";
        return BAD_REQUEST;
    }
    *temp++ = '\0';
    char* method = text;
    if ( strcasecmp( method, "GET" ) == 0 ) {//忽略大小写比较字符串
    	//占时只支持get请求
        httpMethod = GET;
    }
    else {
		LOG(INFO) << "不支持:" << method << " 请求";
        return BAD_REQUEST;
    }
    temp += strspn( temp, " \t" );//返回字符串中第一个不在指定字符串中出现的字符下标
    httpVer = strpbrk( temp, " \t" );
    if ( ! httpVer ) {
        return BAD_REQUEST;
    }
    *httpVer++ = '\0';
    httpVer += strspn( httpVer, " \t" );
    if ( strcasecmp( httpVer, "HTTP/1.1" ) != 0 && strcasecmp( httpVer, "HTTP/1.0" ) != 0) {
    	LOG(INFO) << "不支持:" << httpVer << " http 版本";
        return BAD_REQUEST;
    }

    if ( strncasecmp( temp, "http://", 7 ) == 0 ) {
        temp += 7;
        temp = strchr( temp, '/' );
    }
    else if( strncasecmp( temp, "https://", 8 ) == 0 ){
    	temp += 8;
        temp = strchr( temp, '/' );//查找字符串_Str中首次出现字符_Val的位置
    }

    if ( ! temp || temp[ 0 ] != '/' ) {
        return BAD_REQUEST;
    }
	++temp;//这样可以去掉 /

	//temp后面的就是文件名了
	fileName = httpFileRoot;

	/* 注意：我们默认请求的是 index.html */
	if(*temp == '\0'){
		fileName += "index.html";
	}
	else{
		fileName += temp;
	}
	// LOG(DEBUG) << "fileName = " << fileName << " temp = " << *temp;

	//判断需不需要重定位--因为文件的传输不用当前服务器
	//if(strstr(temp, ".exe") || strstr(temp, ".pdf"))
	
    checkState = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
/* 请求消息头-只解析必出现的几个头 */
Nginx::HTTP_CODE Nginx::ParseRequestHeader(char *text){
	if( text[ 0 ] == '\0' ) {
        if ( httpMethod == HEAD ) {
            return GET_REQUEST;
        }
        if ( contentLength != 0 ) {
            checkState = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            keepLinger = true;
        }
    }
    else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        text += 15;
        text += strspn( text, " \t" );
        contentLength = atol( text );
    }
    else if ( strncasecmp( text, "Host:", 5 ) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        httpHost = text;
    }
    else {
        // LOG(DEBUG) << "未识别的行:" << text;
    }
    return NO_REQUEST;//请求不完整
}
/* 消息体 */
Nginx::HTTP_CODE Nginx::ParseRequestContent(char *text){
	/* 已经读到数据 >= 请求消息体长度 + 已经解析的长度 (消息体是不需要解析的) */
    if ( readIdx >= ( contentLength + checkedIdx ) ) {
        text[ contentLength ] = '\0';
        return GET_REQUEST;
    }
	/* 数据不完整还需要读 */
    return NO_REQUEST;
}

Nginx::LINE_STATUS Nginx::ParseBlankLine(){
	char temp;
    for ( ; checkedIdx < readIdx; ++checkedIdx ) {
        temp = readBuf[ checkedIdx ];
        if ( temp == '\r' ) {
            if ( checkedIdx + 1 > readIdx ) {
                return LINE_OPEN;//数据不完整（有可能下一次读取能读到一个'\n'）
            }
			/* 如果是以 \r\n 结尾说明读到了完整行 */
            else if ( readBuf[ checkedIdx + 1 ] == '\n' ) {
                readBuf[ checkedIdx++ ] = '\0';
                readBuf[ checkedIdx++ ] = '\0';
                return LINE_OK;
            }
			/* 除此之外都是行出错 */
            return LINE_BAD;
        }
		/* 正如上面那样，有可能这一次读到的就是'\n' */
        else if( temp == '\n' ) {
            if( ( checkedIdx > 1 ) && ( readBuf[ checkedIdx - 1 ] == '\r' ) ) {
                readBuf[ checkedIdx - 1 ] = '\0';
                readBuf[ checkedIdx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
	/* 如果解析到现在还没有\r\n 说明数据还不完整 */
    return LINE_OPEN;
}

Nginx::HTTP_CODE Nginx::DoRequest(){
	//提供文件名字，获取文件对应属性。
    if ( stat( fileName.c_str(), &fileStat ) < 0 ) {
    	LOG(DEBUG) << clientName << " fd=" << sockfd << " view资源不存在:" << fileName;
        return NO_RESOURCE;//没有资源
    }
    //文件对应的模式，文件，目录    S_IROTH  其他用户具可读取权限
    if ( ! ( fileStat.st_mode & S_IROTH ) ) {
    	LOG(DEBUG) << "客户端对资源没有足够的访问权限";
        return FORBIDDEN_REQUEST;
    }
    //是否为目录
    if ( S_ISDIR( fileStat.st_mode ) ) {
    	LOG(DEBUG) << "访问的是目录";
        return BAD_REQUEST;
    }

    LOG(DEBUG) << clientName << " 请求的文件:" << fileName << " size=" << fileStat.st_size;
    return FILE_REQUEST;
}

bool Nginx::WriteHttpResponse(){
	/* 先写头信息 */
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
		if(writeIdx == writeLen){
			break;
		}
		else if(writeIdx >  writeLen){
			LOG(WARNING) << "WriteHttpResponse writeIdx >  writeLen " << writeIdx - writeLen;
			return true;
		}
	}
	if(fileName.empty()){
		return true;
	}
	/* 然后写文件 - 我直接用sendfile发送 - 将来如果性能差 - 改为保存到内存 */
	int fd = open(fileName.c_str(), O_RDONLY);
	if(-1 == fd) {
		LOG(WARNING) << "WriteHttpResponse open " << fileName << " error";
		return true;
	}
	if(writeIdx == writeLen){
		writeLen += fileStat.st_size;
	}
	off_t idx = writeIdx - (writeLen - fileStat.st_size);
	while (1){
		// ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
		n = sendfile(sockfd, fd, &idx, writeLen - writeIdx);
		if ( n <= -1 ) {
			if( errno == EAGAIN ) {
				LOG(DEBUG) << "当前不可写，继续监听写事件";
				return false;
			}
			LOG(INFO) << "发送失败";
			return true;
		}
		writeIdx += n;
		if(writeIdx == writeLen){
			break;
		}
		else if(writeIdx >  writeLen){
			LOG(WARNING) << "WriteHttpResponse writeIdx >  writeLen " << writeIdx - writeLen;
			return true;
		}
	}
	writeLen = 0;
	writeIdx = 0;
	close(fd);
	//LOG(DEBUG) << "写:" << writeBuf << "-" << writeLen << "字节";
	return true;
}

//写响应 行 头 体
bool Nginx::WriteHttpHeader(HTTP_CODE ret){
	//修改写指针
	writeIdx = 0;
	writeLen = 0;
	switch ( ret )
    {
        case INTERNAL_ERROR:
        {
            AddStatusLine( 500, error_500_title );
            AddHeaders( strlen( error_500_form ) );
            if ( ! AddContent( error_500_form ) ) {
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            AddStatusLine( 400, error_400_title );
            AddHeaders( strlen( error_400_form ) );
            if ( ! AddContent( error_400_form ) ) {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            AddStatusLine( 404, error_404_title );
            AddHeaders( strlen( error_404_form ) );
            if ( ! AddContent( error_404_form ) ) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            AddStatusLine( 403, error_403_title );
            AddHeaders( strlen( error_403_form ) );
            if ( ! AddContent( error_403_form ) ) {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            AddStatusLine( 200, ok_200_title );
            if ( fileStat.st_size != 0 ) {
                AddHeaders( fileStat.st_size );
                return true;
            }
            else
            {
                const char* ok_string = "<html><body><p>帅东</p></body></html>";
                AddHeaders( strlen( ok_string ) );
                if ( ! AddContent( ok_string ) )
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }
    return true;
}
bool Nginx::AddResponse( const char* format, ... ) {
    if( writeLen >= writeBufSize ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( writeBuf + writeLen, writeBufSize - 1 - writeLen, format, arg_list );
    if( len >= ( writeBufSize - 1 - writeLen ) ) {
        return false;
    }
    writeLen += len;
    va_end( arg_list );
    return true;
}
//添加请求消息行
bool Nginx::AddStatusLine( int status, const char* title ) {
    return AddResponse( "%s %d %s\r\n", "HTTP/1.1", status, title );
}
//添加请求消息头
bool Nginx::AddHeaders( int fileLen, const char *location) {
    AddContentLength( fileLen );
    AddLinger();
	//支持重定向技术
	if(location)
		AddLocation(location);
    AddBlankLine();
}
//内容长度
bool Nginx::AddContentLength( int fileLen ) {
    return AddResponse( "Content-Length: %d\r\n", fileLen );
}
//keep-alive
bool Nginx::AddLinger() {
    return AddResponse( "Connection: %s\r\n", ( keepLinger == true ) ? "keep-alive" : "close" );
}
//重定向
bool Nginx::AddLocation(const char *otherUrl) {
	return AddResponse( "Location: %s\r\n", otherUrl );
}
//添加空行
bool Nginx::AddBlankLine() {
    return AddResponse( "%s", "\r\n" );
}
//添加请求正文(用于错误响应)
bool Nginx::AddContent( const char* content ) {
    return AddResponse( "<html><body><p>%s</p></body></html>", content );
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
			LOG(INFO) << "fd=" << sockfd << " len too long :" << len;
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
		// LOG(DEBUG) << "读到" << clientName << "：" << readBuf << "---" << readIdx << "字节";
		//执行回调函数
		(this->*(callBack[cmd]))(readBuf + 2 * sizeof(int));
		//数据读完 读指针置0
		readIdx = 0;
	}
	readIdx = 0;
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
		if(writeIdx == writeLen){
			// 写完了要监听读事件
			Addfd2Read();
			break;
		}
		else if(writeIdx >  writeLen){
			LOG(WARNING) << "Write writeIdx >  writeLen " << writeIdx - writeLen;
			return true;
		}
	}
	writeLen = 0;
	writeIdx = 0;
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
	struct sockaddr_in clientAddr;
	string ipStr, portStr;
	int connfd;
	socklen_t cliAddrLen;
	while((connfd = accept( dbNginx->lisClifd, (struct sockaddr*)&clientAddr, &cliAddrLen)) > 0) {
		ipStr = string(inet_ntoa(clientAddr.sin_addr));
		portStr = to_string(ntohs(clientAddr.sin_port));
		//name
		dbNginx->nginxs[connfd].clientName = ipStr + " " + portStr;
		//记录fd
		dbNginx->nginxs[connfd].sockfd = connfd;
		//设置超时时间
		dbNginx->nginxs[connfd].SetTimeout(1, 3);//心跳1s、断开3s
		//监听读事件
		dbNginx->nginxs[connfd].Addfd2Read();
		// LOG(DEBUG) << "connect a client: " << clientName << " fd=" << connfd;
	}
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

	checkedIdx = 0;
    startLine = 0;
    fileName.clear();
    httpVer = NULL;
    contentLength = 0;
    keepLinger = false;
    httpHost.clear();
    checkState = CHECK_STATE_REQUESTLINE;
}

void Nginx::ClearResponse(){
	readIdx = 0;
	checkedIdx = 0;
    startLine = 0;
    httpVer = NULL;
    contentLength = 0;
    keepLinger = false;
    httpHost.clear();
    checkState = CHECK_STATE_REQUESTLINE;
}

void Nginx::AcceptNginx(int sockfd){
	/* 除了启动的时候加入对方fd  连接的时候也要加 */
	//记录集群存活的节点fd
	dbNginx->aliveNginxfd.push_back(sockfd);
	//记录fd
	this->sockfd = sockfd;
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