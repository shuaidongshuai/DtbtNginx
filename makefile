GTEST_CPPFLAG1=-IconsHash
GTEST_CPPFLAG2=-Ieasyloggingpp
GTEST_CPPFLAG3=-Iproto
LIB=lib
object=DtbtNginx.o Nginx.o inNginx.pb.o ConsistentHash.o md5.o MD5HashFunc.o Node.o easylogging++.o 
myDtbtNginx: $(object)
	g++ -std=c++11 -o myDtbtNginx $(LIB)/DtbtNginx.o $(LIB)/Nginx.o $(LIB)/inNginx.pb.o \
	$(LIB)/ConsistentHash.o $(LIB)/md5.o $(LIB)/MD5HashFunc.o $(LIB)/Node.o $(LIB)/easylogging++.o -lprotobuf

DtbtNginx.o:
	g++ -std=c++11 -g -c src/DtbtNginx.cc -o $(LIB)/DtbtNginx.o $(GTEST_CPPFLAG1) $(GTEST_CPPFLAG2) $(GTEST_CPPFLAG3)
Nginx.o:
	g++ -std=c++11 -g -c src/Nginx.cc -o $(LIB)/Nginx.o $(GTEST_CPPFLAG1)$(GTEST_CPPFLAG2) $(GTEST_CPPFLAG3)

inNginx.pb.o:
	g++ -std=c++11 -g -c proto/inNginx.pb.cc -o $(LIB)/inNginx.pb.o
	
ConsistentHash.o:
	g++ -std=c++11 -g -c consHash/ConsistentHash.cpp -o $(LIB)/ConsistentHash.o
md5.o:
	g++ -std=c++11 -g -c consHash/md5.cpp -o $(LIB)/md5.o
MD5HashFunc.o:
	g++ -std=c++11 -g -c consHash/MD5HashFunc.cpp -o $(LIB)/MD5HashFunc.o
Node.o:
	g++ -std=c++11 -g -c consHash/Node.cpp -o $(LIB)/Node.o

easylogging++.o:
	g++ -std=c++11 -g -c easyloggingpp/easylogging++.cc -o $(LIB)/easylogging++.o

	
.PHONY:clean
clean:
	rm -f myDtbtNginx $(LIB)/*.o
