"""Fake Gozero ORU pool for RgrMiner tests (getblocktemplate with gozero fields + submitblock shares, HTTPS).
   usage: python3 fake_pool.py <port> <block-bits-hex> <cert.pem> <key.pem>
   Checks every share: auth user, workid, structure, merkle root, time window, share target; counts blocks."""
import hashlib, json, sys, time, struct, base64, threading, ssl, os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
PORT, BITS = int(sys.argv[1]), int(sys.argv[2], 16)
CERT, KEY = sys.argv[3], sys.argv[4]
POOL_ADDR = 'rg1qpoolpoolpoolpoolpoolpoolpoolpoolpoolpo'
CB = "020000000001010000000000000000000000000000000000000000000000000000000000000000ffffffff0402c90300ffffffff020080f0fa0200000016001411111111111111111111111111111111111111110000000000000000266a24aa21a9ede2f61c3f71d1defd3fa999dfa36953755c690689799962b48bebd836974e8cf90120000000000000000000000000000000000000000000000000000000000000000000000000"
def d256(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()
def strip(tx):
    b = bytes.fromhex(tx)
    return b[:4] + b[6:6+1+32+4+1+4+4] + b[6+1+32+4+1+4+4:6+1+32+4+1+4+4+1+8+1+22+8+1+38] + b[-4:]
CBID = d256(strip(CB))
TXS = [bytes([i]) * 60 for i in (1, 2)]
TXIDS = [d256(t) for t in TXS]
def merkle(leaves):
    while len(leaves) > 1:
        if len(leaves) % 2: leaves.append(leaves[-1])
        leaves = [d256(leaves[i] + leaves[i+1]) for i in range(0, len(leaves), 2)]
    return leaves[0]
def target(bits): return (bits & 0x7fffff) * 256 ** ((bits >> 24) - 3)
S = {'prev': bytes(range(32)), 'height': 1000, 'ok': 0, 'rej': 0, 'blocks': 0, 'works': {}, 'lock': threading.Lock(), 'seen': set(), 'users': set()}
def template(share_work):
    now = int(time.time())
    st = min(2**256 // max(int(share_work), 1), 2**256 - 1)
    wid = os.urandom(16).hex()
    S['works'][wid] = (st, S['prev'])
    return {"version": 0x20000000, "previousblockhash": S['prev'][::-1].hex(), "transactions":
            [{"data": t.hex(), "txid": i[::-1].hex(), "hash": i[::-1].hex()} for t, i in zip(TXS, TXIDS)],
            "coinbasevalue": 12800000000, "target": target(BITS).to_bytes(32, 'big').hex(), "mintime": now - 600,
            "maxtime": now + 7200, "mutable": ["time"], "curtime": now, "bits": "%08x" % BITS, "height": S['height'],
            "default_coinbase_tx": CB, "default_coinbase_txid": CBID[::-1].hex(), "merkleroot": merkle([CBID] + TXIDS)[::-1].hex(),
            "coinbase_address": POOL_ADDR, "rigor_coinbase_immutable": True, "workid": wid, "gozero_pool": 1,
            "gozero_share_target": st.to_bytes(32, 'big').hex(), "gozero_pool_address": POOL_ADDR, "gozero_fee_bps": 100,
            "expires": 120, "gozero_requested_work": int(share_work), "gozero_share_work": int(share_work) - 1}
def check(hexblk, wid):
    if wid not in S['works']: return 'unknown-work'
    st, prev = S['works'][wid]
    b = bytes.fromhex(hexblk); hdr = b[:80]
    if hdr[4:36] != prev: return 'bad-prevblk'
    if prev != S['prev']: return 'stale-prevblk'
    if b[80] != 3 or b[81:81+len(bytes.fromhex(CB))] != bytes.fromhex(CB) or b[81+len(bytes.fromhex(CB)):] != b''.join(TXS): return 'bad-block-structure'
    if hdr[36:68] != merkle([CBID] + TXIDS): return 'bad-txnmrklroot'
    ntime, bits, nonce = struct.unpack('<III', hdr[68:80])
    if not (time.time() - 600 <= ntime <= time.time() + 7200): return 'time-invalid'
    if hdr in S['seen']: return 'duplicate'
    pw = int.from_bytes(hashlib.sha3_256(b'RGRPOW01' + hdr).digest(), 'little')
    if pw > st: return 'low-difficulty-share'
    S['seen'].add(hdr)
    if pw <= target(BITS):
        S['blocks'] += 1; S['prev'] = d256(hdr); S['height'] += 1
        print('BLOCK', S['blocks'], flush=True)
    return None
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        auth = self.headers.get('Authorization', '')
        try: user = base64.b64decode(auth.split()[1]).decode().split(':')[0]
        except Exception: user = ''
        if not user.startswith('rg1q') or '.' not in user:
            self.send_response(401); self.end_headers(); return
        S['users'].add(user)
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        m, p = req['method'], req.get('params', [])
        res, err = None, None
        with S['lock']:
            if m == 'getblocktemplate':
                o = p[0] if p else {}
                if o.get('gozero_pool') != 1: err = {"code": -1, "message": "gozero_pool required"}
                else: res = template(o.get('gozero_share_work', 1e9))
            elif m == 'submitblock':
                wid = p[1].get('workid') if len(p) > 1 and isinstance(p[1], dict) else None
                res = check(p[0], wid)
                if res is None: S['ok'] += 1
                else: S['rej'] += 1; print('rejected', res, flush=True)
            else: err = {"code": -32601, "message": "Method not found"}
        out = json.dumps({"result": res, "error": err, "id": req.get('id')}).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(out))); self.end_headers(); self.wfile.write(out)
def report():
    while True:
        time.sleep(5); print('STATS ok', S['ok'], 'rej', S['rej'], 'blocks', S['blocks'], 'users', sorted(S['users']), flush=True)
threading.Thread(target=report, daemon=True).start()
srv = ThreadingHTTPServer(('127.0.0.1', PORT), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(CERT, KEY)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
