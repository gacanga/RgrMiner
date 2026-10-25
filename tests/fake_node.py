"""Fake rigord for RgrMiner tests: getblocktemplate (template + proposal), submitblock, getblockcount.
   Checks every submitted block like the node would (structure, merkle root, time window, PoW).
   usage: python3 fake_node.py <port> <bits-hex> <address>"""
import hashlib, json, sys, time, struct, base64, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
PORT, BITS, ADDR = int(sys.argv[1]), int(sys.argv[2], 16), sys.argv[3]
USER, PASS = 'tuser', 'tpass'
# coinbase in the format of a real mainnet block (height 969, dummy payout key hash 11..11), + 2 fake transactions
CB = "020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff0402c90300ffffffff020080f0fa0200000016001411111111111111111111111111111111111111110000000000000000266a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf90120000000000000000000000000000000000000000000000000000000000000000000000000"
def d256(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()
def strip(tx):
    b = bytes.fromhex(tx)
    # coinbase above: version | 00 01 | 1 input | 2 outputs | witness | locktime
    assert b[4:6] == b'\x00\x01'
    return b[:4] + b[6:6+1+32+4+1+4+4] + b[6+1+32+4+1+4+4:6+1+32+4+1+4+4+1+8+1+22+8+1+38] + b[-4:]
CBID = d256(strip(CB))
TXS = [bytes([i]) * 60 for i in (1, 2)]
TXIDS = [d256(t) for t in TXS]
def merkle(leaves):
    while len(leaves) > 1:
        if len(leaves) % 2: leaves.append(leaves[-1])
        leaves = [d256(leaves[i] + leaves[i+1]) for i in range(0, len(leaves), 2)]
    return leaves[0]
def target(bits):
    e, m = bits >> 24, bits & 0x7fffff
    return m * 256 ** (e - 3)
state = {'prev': bytes(range(32)), 'height': 1000, 'accepted': 0, 'rejected': 0, 'lock': threading.Lock()}
def template():
    now = int(time.time())
    return {"version": 0x20000000, "previousblockhash": state['prev'][::-1].hex(), "transactions":
            [{"data": t.hex(), "txid": i[::-1].hex(), "hash": i[::-1].hex()} for t, i in zip(TXS, TXIDS)],
            "coinbasevalue": 12800000000, "longpollid": state['prev'][::-1].hex() + "1",
            "target": target(BITS).to_bytes(32, 'big').hex(), "mintime": now - 600, "maxtime": now + 7200,
            "mutable": ["time"], "curtime": now, "bits": "%08x" % BITS, "height": state['height'],
            "default_coinbase_tx": CB, "default_coinbase_txid": CBID[::-1].hex(),
            "merkleroot": merkle([CBID] + TXIDS)[::-1].hex(), "coinbase_address": ADDR, "rigor_coinbase_immutable": True}
def check_block(hexblk, pow_required=True):
    b = bytes.fromhex(hexblk)
    hdr = b[:80]
    ver, = struct.unpack('<I', hdr[:4]); prev = hdr[4:36]; mr = hdr[36:68]
    ntime, bits, nonce = struct.unpack('<III', hdr[68:80])
    if prev != state['prev']: return 'bad-prevblk'
    if b[80] != 3: return 'bad-txcount'
    body = b[81:]
    cb = bytes.fromhex(CB)
    if body[:len(cb)] != cb: return 'bad-coinbase'
    if body[len(cb):] != b''.join(TXS): return 'bad-txns'
    if mr != merkle([CBID] + TXIDS): return 'bad-txnmrklroot'
    now = int(time.time())
    if not (now - 600 <= ntime <= now + 7200): return 'time-invalid'
    if bits != BITS: return 'bad-diffbits'
    pw = hashlib.sha3_256(b'RGRPOW01' + hdr).digest()
    if pow_required and int.from_bytes(pw, 'little') > target(BITS): return 'high-hash'
    return None
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        if self.headers.get('Authorization') != 'Basic ' + base64.b64encode(f'{USER}:{PASS}'.encode()).decode():
            self.send_response(401); self.end_headers(); return
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        m, p = req['method'], req.get('params', [])
        res, err = None, None
        with state['lock']:
            if m == 'getblocktemplate':
                o = p[0] if p else {}
                if o.get('mode') == 'proposal': res = check_block(o['data'], pow_required=False)
                elif o.get('coinbase_address') != ADDR: err = {"code": -5, "message": "bad coinbase_address"}
                else: res = template()
            elif m == 'submitblock':
                res = check_block(p[0])
                if res is None:
                    state['accepted'] += 1; state['prev'] = d256(bytes.fromhex(p[0])[:80]); state['height'] += 1
                    print(f"accepted block {state['accepted']} nonce-time {bytes.fromhex(p[0])[68:72].hex()}", flush=True)
                else:
                    state['rejected'] += 1; print('rejected', res, flush=True)
            elif m == 'getblockcount': res = state['height'] - 1
            else: err = {"code": -32601, "message": "Method not found"}
        out = json.dumps({"result": res, "error": err, "id": req.get('id')}).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(out))); self.end_headers(); self.wfile.write(out)
ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
