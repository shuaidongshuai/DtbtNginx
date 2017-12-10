/* proto的回调函数 */
//inNginx
enum inNginxNum{
	VoteNo = 1,				//	Vote
	AckVote2LeaderNo,		//	AckVote2Leader
	AckVote2FollowerNo,		//	AckVote2Follower
	
	SynchDataNo,			//	SynchData
	AckData2LeaderNo,		//	AckData2Leader
	AckData2FollowerNo,		//	AckData2Follower
	
	KeepAliveNo,			//	KeepAlive

	SerConNo,				//	SerCon
	CliConNo,				//	CliCon

	Server2NginxNo,			//	ServerRcve
};