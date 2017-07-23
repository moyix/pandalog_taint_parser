CXXFLAGS=-pthread -std=c++11 -O3 -ggdb
LDFLAGS=-L/usr/local/lib
LIBS=-lz -lprotobuf -lpthread

all: label_tcn label_pcs

plog.pb.cc plog.pb.h: plog.proto
	protoc --cpp_out=. $<

plog.pb.o: plog.pb.cc plog.pb.h

label_tcn.o: plog.pb.h label_tcn.cpp

label_pcs.o: plog.pb.h label_pcs.cpp

label_tcn: label_tcn.o plog.pb.o
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

label_pcs: label_pcs.o plog.pb.o
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

clean:
	rm -f *.o plog.pb.cc plog.pb.h label_pcs label_tcn
