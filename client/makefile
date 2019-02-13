CXX=g++
CXXFLAGS = -std=c++11
LFLAGS = -lpthread
TARGET = client

all: $(TARGET)

$(TARGET): $(TARGET).cpp
	$(CXX) -o $(TARGET) $(TARGET).cpp $(CXXFLAGS) $(LFLAGS)

clean:
	$(RM) *.o *~ $(TARGET)
