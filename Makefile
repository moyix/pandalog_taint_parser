CXXFLAGS=-pthread -std=c++11 -O3 -ggdb
LDFLAGS=-L/usr/local/lib
LIBS=-lz -lprotobuf -lpthread

all: get_ulps

plog.pb.cc plog.pb.h: plog.proto
	protoc --cpp_out=. $<

plog.pb.o: plog.pb.cc plog.pb.h

get_ulps.o: plog.pb.h get_ulps.cpp

get_ulps: get_ulps.o plog.pb.o
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f *.o plog.pb.cc plog.pb.h
