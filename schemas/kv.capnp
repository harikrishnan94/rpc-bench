@0xc0ed9b2ebc964182;

# rpc-bench uses one small RPC surface so the wire contract stays easy to audit
# while benchmarking different client and server concurrency settings.
interface KvService {
  get @0 (key :Data) -> (found :Bool, value :Data);
  put @1 (key :Data, value :Data) -> ();
  delete @2 (key :Data) -> (found :Bool);
}
