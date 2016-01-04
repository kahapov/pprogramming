#include <exception>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sys/select.h>

#include "master.h"

const unsigned ALIVE_CELL_PROBABILITY = 69; 

using std::exception;
using std::vector;
using std::shared_ptr;
using std::cin;

char Stream[100000];
char* Cur = Stream;
char* StreamEnd = Stream;

void PrintField(const vector<vector<int>> &f) {
  for (size_t i = 1; i < f.size() - 1;++i)
  {
    for (size_t j = 0; j < f[0].size(); ++j)
    {
      printf("%d ",f[i][j]);   
    }
    printf("\n");
  }
}

void BaseMaster::HandleRequest()
{
  if (State == RUNNING)
  {
    Calculate();
    CollabSync();
    SendCalculations();
    ReceiveCalculations();
    IterNumber++;
    if (IterNumber == IterCount)
    {
      WorkersSync();
      SendFinalReport();
      State = STOPPED;
      ClearRequests();
    }
  }

  if (Requests.empty())
  {
    WorkersSync();
    WorkersSync();
    return;
  }

  if (Requests.front() == "RUN" && (State == STOPPED || State == WAITING))
  {
    IterNumber = 0;
    IterCount = Requests.front().GetIterCount();
    State = RUNNING;
    SendRequest(Requests.front());
    Requests.pop_front();
    return;
  }
  

  if (Requests.front() == "STOP" && State == RUNNING)
  { 
    SendRequest(Requests.front());
    WorkersSync();
    SendFinalReport();
    State = STOPPED;
    ClearRequests();
  }
  else if (Requests.front() == "QUIT" && State == STOPPED)
  {
    SendRequest(Requests.front());
    State = ENDED;
  } 
  else if (State == WAITING && Requests.front() == "START")
  {
    GetField(); 
    InitWorkers();
    Requests.pop_front();
  } 
  else 
    HandleOtherRequests();
}

void LocalMaster::GetField(){
  GetRandomField();
}

void LocalMaster::GetRandomField()
{
  srand(time(0));
  OldField.resize(Height + 2, vector<int>(Width));

  OldField.resize(Height + 2, vector<int>(Width));
  for (size_t i = 1; i < OldField.size() - 1; ++i)
    for (size_t j = 0; j < OldField[0].size(); ++j)
      OldField[i][j] = rand()%100 < ALIVE_CELL_PROBABILITY ? 1 : 0;
  OldField[0] = OldField[OldField.size() - 2];
  OldField[OldField.size() - 1] = OldField[1];
  Field = OldField;
} 

void LocalMaster::SendRequest(BaseRequest req)
{
  WorkersSync();
  RequestsToSend.push_back(req);  
  WorkersSync();
}

void LocalMaster::SendFinalReport()
{
  printf("Done\n");
  for (size_t i = 0; i < WorkersCount; ++i) 
  {
    size_t sz = (Height - 1) / WorkersCount + 1;
    size_t y0 = sz * i;
    for (size_t y = 1; y < std::min(sz + 2, Height + 2 - y0) - 1; ++y)
    {
      Field[y0 + y] = FieldsToSend[i][y]; 
    }
  }
  PrintField(Field);
}

void LocalMaster::TakeRequests() 
{
  bool Passed = false;
  while (!Passed || (State != RUNNING && Requests.empty())) 
  {
    int Read = read(STDIN_FILENO, StreamEnd, 100);
    if (Read > 0) 
      StreamEnd+=Read;
    if (!strncmp(Cur, "HELP", 4)) 
    {
      PrintHelpMessage();
      Cur+=5;
    }
    else if (!strncmp(Cur, "RUN", 3)) {
      Cur+=4;
      int ic; 
      sscanf(Cur, "%d", &ic);
      while (*Cur != '\n')
        Cur++;
      Cur++;
      Requests.push_back(BaseRequest("RUN",ic));
    }  
    else if (!strncmp(Cur, "STATUS", 6))
    {
      Cur += 7;
      Requests.push_back(BaseRequest("STATUS"));
    }
    else if (!strncmp(Cur, "QUIT", 4))
    {
      Cur += 5;
      Requests.push_back(BaseRequest("QUIT"));
    }
    else if (!strncmp(Cur, "STOP", 4))
    {
      Cur += 5;
      Requests.push_back(BaseRequest("STOP"));
    }
    else if (!strncmp(Cur, "START", 5))
    { 
      Cur+=6;
      if (!Height || !Width) { 
        sscanf(Cur, "%u%u", &Height, &Width);
        if (!Height || !Width)
          throw std::logic_error("Wrong field size");
        while (*Cur != '\n')
          Cur++;
        Cur++;
        Slaves.resize(WorkersCount);
        FieldsToSend.resize(WorkersCount);
        Requests.push_back(BaseRequest("START"));
      }
    }
    else if (StreamEnd - Cur > 1) {
      /*printf("!");
      putchar(*Cur);
      printf("!\n");*/
      Cur++;
    } 
    Passed = true;
  }
}

void ThreadMaster::InitWorkers() {
  PrintField(Field);
  ThreadWorkerDataCommon threadCommon;
  threadCommon.WorkersBarrier = &WorkersBarrier;
  threadCommon.MasterBarrier = &MasterBarrier;
  for (size_t i = 0; i < WorkersCount; ++i) 
  {
    size_t sz = (Height - 1) / WorkersCount + 1;
    size_t y0 = sz * i;
    FieldsToSend[i].resize(std::min(sz + 2 ,Height + 2 - y0));
    for (size_t y = 0; y < std::min(sz + 2, Height + 2 - y0); ++y)
    {
      FieldsToSend[i][y] = Field[y0 + y]; 
    }
  }
  for (size_t i = 0; i < WorkersCount; ++i)
  {
    LocalWorkerData localData;
    localData.SrcField = &FieldsToSend[i];
    localData.SendFieldBottom = &FieldsToSend[(i + 1) % WorkersCount][0]; 
    localData.SendFieldTop = &FieldsToSend[(i + WorkersCount - 1) % WorkersCount].back();
    localData.ReceiveFieldBottom = &FieldsToSend[i][FieldsToSend[i].size() - 1];
    localData.ReceiveFieldTop = &FieldsToSend[i][0];
    localData.RequestsFromMaster = &RequestsToSend;
    localData.RequestQueuePosition = 0;
    std::shared_ptr<ThreadWorker> tmp(new ThreadWorker(i, localData, threadCommon));
    Slaves[i] = tmp;
  }
}

void ThreadMaster::WorkersSync()
{
  pthread_barrier_wait(&MasterBarrier);
}

ThreadMaster::ThreadMaster(int WorkersCount_)
{
  WorkersCount = WorkersCount_;
  PrintHelpMessage();
  pthread_barrier_init(&WorkersBarrier, nullptr, WorkersCount);
  pthread_barrier_init(&MasterBarrier, nullptr, WorkersCount + 1);
  int flag = fcntl(STDIN_FILENO, F_GETFL);
  fcntl(STDIN_FILENO, F_SETFL, flag | O_NONBLOCK);
  State = WAITING; 
}

ThreadMaster::~ThreadMaster() 
{
  int flag = fcntl(STDIN_FILENO, F_GETFL);
  fcntl(STDIN_FILENO, F_SETFL, flag ^ O_NONBLOCK);
  pthread_barrier_destroy(&WorkersBarrier);
  pthread_barrier_destroy(&MasterBarrier);
}
