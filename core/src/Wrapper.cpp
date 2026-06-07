#include "Dispatcher.h"
#include "ExchangeEngine.h"
#include "NetworkServer.h"
#include "RunContext.h"
#include <thread>

int main() {

  // 1. Boot the untrusted participant engine on the main thread.
  ExchangeEngine *engine = CreateEngine();
  engine->Init();

  // 2. Start infrastructure threads 
  RunContext ctx;
  std::thread dispatcher_thread(RunDispatcher, engine, std::ref(ctx));
  std::thread network_thread(RunNetworkServer, std::ref(ctx));

  network_thread.join();
  dispatcher_thread.join();

  delete engine;
  return 0;
}
