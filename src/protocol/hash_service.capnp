@0xfcaa4ee6d65c72a2;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("rpcbench");

# Internal Cap'n Proto RPC surface for the CRC32 benchmark service.
interface HashService {
  # Returns the IEEE CRC32 of the request payload bytes.
  hash @0 (payload :Data) -> (crc32 :UInt32);
}
