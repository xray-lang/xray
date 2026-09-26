"""Compile exact native leaf scalar arguments through the tagged shim ABI."""
from native_source_compile import compile_cases

if __name__ == "__main__":
    raise SystemExit(compile_cases({
        "shutdown": 'import net\nimport { NetConn } from net\nexport fn probe(conn: NetConn) -> bool { return net.shutdown(conn) }',
        "multicast": 'import net\nexport fn probe(group: string, port: i64, ttl: i64, loopback: bool) -> bool { var conn = net.udpMulticastBind(group, port, ttl, loopback); return net.shutdown(conn) }',
        "udp_sender_local": 'import net\nimport { NetConn } from net\nexport fn probe(conn: NetConn) -> string { var buffer: Array<u8> = [0, 0]; var packet = net.recvFrom(conn, ref buffer, 0); if (packet == null) { return "" }; return packet!.host }',
        "udp_sender_forwarded": 'import { receive } from "./udp"\nimport { NetConn } from net\nexport fn probe(conn: NetConn) -> string { var buffer: Array<u8> = [0, 0]; return receive(conn, ref buffer) }',
    }, {
        "udp.xr": 'import net\nimport { NetConn } from net\nexport fn receive(conn: NetConn, buffer: ref Array<u8>) -> string { var packet = net.recvFrom(conn, ref buffer, 0); if (packet == null) { return "" }; return packet!.host }',
    }))
