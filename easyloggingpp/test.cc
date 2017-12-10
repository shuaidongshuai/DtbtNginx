#include "easylogging++.h"  

//使用宏 INITIALIZE_EASYLOGGINGPP 初始化
INITIALIZE_EASYLOGGINGPP

int main(int argc, char** argv)  
{  
    el::Configurations conf("../conf/Log.conf");  
    el::Loggers::reconfigureAllLoggers(conf);  
	
    LOG(TRACE)   << "***** trace log  *****";  
    LOG(DEBUG)   << "***** debug log  *****";  
    LOG(ERROR)   << "***** error log  *****";  
    LOG(WARNING) << "***** warning log  *****";  
    LOG(INFO)    << "***** info log  *****";  
  
    return 0;  
}  