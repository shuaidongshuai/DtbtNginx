/*1，在长连接下，有可能很长一段时间都没有数据往来。
理论上说，这个连接是一直保持连接的，但是实际情况中，如果中间节点出现什么故障是难以知道的。
有的节点（防火墙）会自动把一定时间之内没有数据交互的连接给断掉。
在这个时候，就需要我们的心跳包了，用于维持长连接，保活

2，心跳包之所以叫心跳包是因为：它像心跳一样每隔固定时间发一次，以此来告诉服务器，这个客户端还活着。
事实上这是为了保持长连接，至于这个包的内容，是没有什么特别规定的，不过一般都是很小的包，或者只包含包头的一个空包。
心跳包主要也就是用于长连接的保活和断线处理。一般的应用下，判定时间在30-40秒比较不错。如果实在要求高，那就在6-9秒。

3，下面为封装好的心跳包函数，加入项目中参数设置一下即可
*/
#include "common.h"
//参数解释
//fd:网络连接描述符
//start:首次心跳侦测包发送之间的空闲时间  
//interval:两次心跳侦测包之间的间隔时间 
//count:探测次数，即将几次探测失败判定为TCP断开
int set_tcp_keepAlive(int fd, int start, int interval, int count)  
{  
    int keepAlive = 1;  
    if (fd < 0 || start < 0 || interval < 0 || count < 0) return -1;  //入口参数检查 ，编程的好习惯。
    //启用心跳机制，如果您想关闭，将keepAlive置零即可  
    if(setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,(void*)&keepAlive,sizeof(keepAlive)) == -1)  
    {  
        perror("setsockopt");  
        return -1;  
    }  
    //启用心跳机制开始到首次心跳侦测包发送之间的空闲时间  
    if(setsockopt(fd,SOL_TCP,TCP_KEEPIDLE,(void *)&start,sizeof(start)) == -1)  
    {  
        perror("setsockopt");  
        return -1;  
    }
    //两次心跳侦测包之间的间隔时间  
    if(setsockopt(fd,SOL_TCP,TCP_KEEPINTVL,(void *)&interval,sizeof(interval)) == -1)  
    {  
        perror("setsockopt");
        return -1;  
    }  
    //探测次数，即将几次探测失败判定为TCP断开  
    if(setsockopt(fd,SOL_TCP,TCP_KEEPCNT,(void *)&count,sizeof(count)) == -1)  
    {  
        perror("setsockopt");  
        return -1;  
    }  
    return 0;  
}
/*
将想设置的参数传入该函数，设置成功返回0，否则返回-1。设置成功以后，可以将fd交给select去监听可读可写事件，如果select检测到fd可读且read返回错误，一般就能判定该fd对应的TCP连接已经异常断开，调用close函数将fd关闭即可。

TCP连接非正常断开的检测(KeepAlive探测)

此处的”非正常断开”指TCP连接不是以优雅的方式断开,如网线故障等物理链路的原因,还有突然主机断电等原因

有两种方法可以检测:1.TCP连接双方定时发握手消息 2.利用TCP协议栈中的KeepAlive探测

第二种方法简单可靠,只需对TCP连接两个Socket设定KeepAlive探测。

从而得知连接已失效，客户端程序便有机会及时执行清除工作、提醒用户或重新连接。

--------------------------------------------------------------------------------------------------------------------------------------------------------------------
int setsockopt(int sockfd, int level, int optname,const void *optval, socklen_t optlen);
sockfd：指向一个打开的套接口描述字
level：选项定义的层次；支持SOL_SOCKET、IPPROTO_TCP、IPPROTO_IP和IPPROTO_IPV6。(级别)： 指定选项代码的类型。
	SOL_SOCKET: 基本套接口
	IPPROTO_IP: IPv4套接口
	IPPROTO_IPV6: IPv6套接口
	IPPROTO_TCP: TCP套接口
optname(选项名)： 选项名称
optval(选项值): 是一个指向变量的指针 类型：整形，套接口结构， 其他结构类型:linger{}, timeval{ }
optlen(选项长度) ：optval 的大小
返回值：标志打开或关闭某个特征的二进制选项

SO_KEEPALIVE 保持连接
	检测对方主机是否崩溃，避免（服务器）永远阻塞于TCP连接的输入。 
	设置该选项后，如果2小时内在此套接口的任一方向都没有数据交换，TCP就自动给对方 发一个保持存活探测分节(keepalive probe)。
	这是一个对方必须响应的TCP分节.它会导致以下三种情况： 对方接收一切正常：以期望的 ACK响应。2小时后，TCP将发出另一个探测分节。
	对方已崩溃且已重新启动：以RST响应。套接口的待处理错误被置为ECONNRESET，套接 口本身则被关闭。 对方无任何响应：
	源自berkeley的TCP发送另外8个探测分节，相隔75秒一个，试图得到 一个响应。在发出第一个探测分节11分钟15秒后若仍无响应就放弃。
	套接口的待处理错 误被置为ETIMEOUT，套接口本身则被关闭。如ICMP错误是“host unreachable (主机不 可达)”，说明对方主机并没有崩溃，
	但是不可达，这种情况下待处理错误被置为 EHOSTUNREACH。

tcp_keepidle 
对一个连接进行有效性探测之前运行的最大非活跃时间间隔，默认值为 14400（即 2 个小时） 

tcp_keepintvl 
两个探测的时间间隔，默认值为 150 即 75 秒 

tcp_keepcnt 
关闭一个非活跃连接之前进行探测的最大次数，默认为 8 次 
--------------------------------------------------------------------------------------------------------------------------------------------------------------------
cat /proc/sys/net/ipv4/tcp_keepalive_time
7200
cat /proc/sys/net/ipv4/tcp_keepalive_intvl
75
cat /proc/sys/net/ipv4/tcp_keepalive_probes
9
以上参数的大致意思是：keepalive routine每2小时（7200秒）启动一次，发送第一个probe（探测包），
如果在75秒内没有收到对方应答则重发probe，当连续9个probe没有被应答时，认为连接已断。（底层会自动断开）
--------------------------------------------------------------------------------------------------------------------------------------------------------------------
read/recv函数返回0均表示正常结束。此时关闭即可。如果用select/poll/epoll管理，该套接字也会读就绪，然后调用recv/read返回0。
  对于异常关闭如网络崩溃、主机宕机等，可通过设置SO_KEEPALIVE设置保活，协议会按照设定间隔自动发送探测分节。
  该选项分为设置无数据首次探测 时间、探测间隔、探测次数控制TCP是否出错。
  如果你设置首次探测在10秒之后、探测间隔3次，探测次数3次，则最多30秒之后将给应用层返回一个对方非 正常关闭的异常，
  此时可通过获得errno得到对应错误 SOCKET_ERROR  ，read/recv返回为-1
*/



