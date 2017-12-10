#ifndef SIGNLETON_H
#define SIGNLETON_H
/* 通用的单例模板 */
template<typename T>
class Singleton
{
private:
	Singleton(){}
	virtual ~Singleton(){}
	static T *instance;
public:
	static T *getInstence(){ return instance; }
};
template<typename T>
T *Singleton<T>::instance = new T;
#endif