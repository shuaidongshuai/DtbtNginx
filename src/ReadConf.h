#ifndef READCONF_H
#define READCONF_H
#include <fstream>
#include <map>
using namespace std;
class ReadConf
{
public:
	map<string, string> conf;
	bool read(string src){
		ifstream infile(src);
		string key, value, temp;
		while(infile >> key){//获取key
			if(key[0] == '#' || key[0] == ' ' || key[0] == '\n')
				continue;
			if(!(infile >> temp))
				return false;
			if(temp != "=")
				return false;
			if(!(infile >> value))
				return false;
			//不能出现相同的key
			if(conf.find(key) == conf.end())
				conf[key] = value;
			else
				return false;
		}
		return true;
	}
};
//没有过多编写这个类，只是简单的利用空格进行提取 key value
#endif