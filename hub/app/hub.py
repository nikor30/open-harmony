"""open-harmony hub: one place for gateways, devices and activities.

Runs as a Home Assistant add-on (ingress panel) or on any machine for
development. It keeps the activities, talks to the IR/RF gateways over
their REST API and WebSocket and calls Home Assistant services, so one
activity can mix infrared, 433 MHz and anything Home Assistant controls.
"""

import asyncio
import json
import logging
import os
import re
import time
from pathlib import Path

import aiohttp
from aiohttp import web

log = logging.getLogger("hub")
HERE = Path(__file__).parent
DATA = Path(os.environ.get("HUB_DATA", "/data"))
STORE = DATA / "hub.json"
PORT = int(os.environ.get("HUB_PORT", "8099"))

if os.environ.get("SUPERVISOR_TOKEN"):
    HA_URL = "http://supervisor/core/api"
    HA_TOKEN = os.environ["SUPERVISOR_TOKEN"]
else:
    HA_URL = os.environ.get("HUB_HA_URL", "").rstrip("/") + "/api" if os.environ.get("HUB_HA_URL") else ""
    tf = os.environ.get("HUB_HA_TOKEN_FILE")
    HA_TOKEN = Path(tf).read_text().strip() if tf else os.environ.get("HUB_HA_TOKEN", "")

# Domains offered in the activity editor; everything else is noise there.
HA_DOMAINS = {"media_player", "light", "switch", "remote", "scene", "script", "input_boolean", "cover", "fan", "climate", "button"}


def slug(s):
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_") or "x"


class Store:
    def __init__(self):
        self.data = {"gateways": [], "activities": [], "current": None}
        if STORE.exists():
            self.data.update(json.loads(STORE.read_text()))

    def save(self):
        DATA.mkdir(parents=True, exist_ok=True)
        tmp = STORE.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, indent=1))
        tmp.replace(STORE)


class Gateway:
    """Mirror of one gateway: status and library, events relayed to the browsers."""

    def __init__(self, hub, cfg):
        self.hub, self.cfg = hub, cfg
        self.id, self.url = cfg["id"], cfg["url"].rstrip("/")
        self.status, self.db, self.online = {}, {"devices": [], "activities": []}, False
        self.task = None

    def view(self):
        return {"id": self.id, "url": self.url, "name": self.status.get("name") or self.id,
                "online": self.online, "status": self.status, "db": self.db}

    async def get(self, path):
        async with self.hub.http.get(self.url + path, timeout=aiohttp.ClientTimeout(total=5)) as r:
            r.raise_for_status()
            return await r.json(content_type=None)

    async def post(self, path, body, method="POST"):
        async with self.hub.http.request(method, self.url + path, json=body, timeout=aiohttp.ClientTimeout(total=8)) as r:
            text = await r.text()
            if r.status >= 300:
                raise web.HTTPBadGateway(text=f"gateway {self.id}: {r.status} {text}")
            return text

    async def refresh(self):
        self.status = await self.get("/api/status")
        self.db = await self.get("/api/db")

    async def run(self):
        ws_url = re.sub(r"^http", "ws", self.url) + "/ws"
        while True:
            try:
                await self.refresh()
                async with self.hub.http.ws_connect(ws_url, heartbeat=20) as ws:
                    self.online = True
                    await self.hub.broadcast({"type": "gateway", "gateway": self.view()})
                    async for msg in ws:
                        if msg.type != aiohttp.WSMsgType.TEXT:
                            continue
                        ev = json.loads(msg.data)
                        if ev.get("type") == "db":
                            self.db = await self.get("/api/db")
                            await self.hub.broadcast({"type": "gateway", "gateway": self.view()})
                        elif ev.get("type") == "status":
                            self.status.update({k: v for k, v in ev.items() if k != "type"})
                        else:
                            await self.hub.broadcast({"type": "gw_event", "gateway": self.id, "event": ev})
            except asyncio.CancelledError:
                raise
            except Exception as e:  # offline, rebooting, OTA ...
                log.info("gateway %s: %s", self.id, e)
            if self.online:
                self.online = False
                await self.hub.broadcast({"type": "gateway", "gateway": self.view()})
            await asyncio.sleep(5)

    async def save_command(self, device, name, code):
        """Add a learned code to the gateway's library (creates the device if needed)."""
        db = await self.get("/api/db")
        dev = next((d for d in db["devices"] if d["id"] == device or d["name"] == device), None)
        if not dev:
            dev = {"id": slug(device), "name": device, "type": "", "commands": []}
            db["devices"].append(dev)
        cid = slug(name)
        dev["commands"] = [c for c in dev["commands"] if c["id"] != cid]
        dev["commands"].append({"id": cid, "name": name, "code": code})
        await self.post("/api/db", db, method="PUT")
        self.db = db
        return {"device": dev["id"], "command": cid}


class Hub:
    def __init__(self):
        self.store = Store()
        self.gateways = {}
        self.clients = set()
        self.runner = None  # (activity id, task)
        self.http = None

    # ------------------------------------------------------------ plumbing

    async def start(self, app):
        self.http = aiohttp.ClientSession()
        for g in self.store.data["gateways"]:
            self.add_gateway(g)

    async def stop(self, app):
        for g in self.gateways.values():
            g.task.cancel()
        await self.http.close()

    def add_gateway(self, cfg):
        g = Gateway(self, cfg)
        g.task = asyncio.create_task(g.run())
        self.gateways[g.id] = g
        return g

    async def broadcast(self, msg):
        s = json.dumps(msg)
        for ws in list(self.clients):
            try:
                await ws.send_str(s)
            except Exception:
                self.clients.discard(ws)

    def state(self):
        d = self.store.data
        return {"gateways": [g.view() for g in self.gateways.values()], "activities": d["activities"],
                "current": d["current"], "running": self.runner[0] if self.runner else None,
                "ha": bool(HA_URL and HA_TOKEN)}

    async def ha(self, method, path, body=None):
        if not HA_URL:
            raise web.HTTPServiceUnavailable(text="Home Assistant not configured")
        async with self.http.request(method, HA_URL + path, json=body, timeout=aiohttp.ClientTimeout(total=10),
                                     headers={"Authorization": "Bearer " + HA_TOKEN}) as r:
            if r.status >= 300:
                raise web.HTTPBadGateway(text=f"Home Assistant: {r.status} {await r.text()}")
            return await r.json(content_type=None)

    # ------------------------------------------------------------ activities

    async def do_step(self, step):
        kind = step.get("do")
        if kind == "send":
            g = self.gateways[step["gateway"]]
            for _ in range(max(1, int(step.get("repeat", 1)))):
                await g.post("/api/send", {"device": step["device"], "command": step["command"]})
        elif kind == "ha":
            domain, service = step["service"].split(".", 1)
            body = dict(step.get("data") or {})
            if step.get("entity_id"):
                body["entity_id"] = step["entity_id"]
            await self.ha("POST", f"/services/{domain}/{service}", body)
        elif kind == "wait":
            await asyncio.sleep(int(step.get("ms", 500)) / 1000)
        else:
            raise ValueError(f"unknown step {kind!r}")

    async def run_steps(self, act_id, phase, steps):
        for i, step in enumerate(steps):
            await self.broadcast({"type": "step", "activity": act_id, "phase": phase, "index": i, "total": len(steps)})
            try:
                await self.do_step(step)
            except asyncio.CancelledError:
                raise
            except Exception as e:  # keep going: one dead device must not stop the rest
                log.warning("%s %s step %d: %s", act_id, phase, i, e)
                await self.broadcast({"type": "step_error", "activity": act_id, "phase": phase, "index": i, "error": str(e)})

    async def switch(self, target):
        """Stop the current activity (if any) and start target (None = all off)."""
        acts = {a["id"]: a for a in self.store.data["activities"]}
        cur = acts.get(self.store.data["current"])
        t0 = time.monotonic()
        if cur and cur["id"] != target:
            await self.run_steps(cur["id"], "stop", cur.get("stop", []))
        if target:
            await self.run_steps(target, "start", acts[target].get("start", []))
        self.store.data["current"] = target
        self.store.save()
        await self.broadcast({"type": "current", "current": target, "ms": int((time.monotonic() - t0) * 1000)})

    def start_switch(self, target):
        if self.runner:
            self.runner[1].cancel()
        task = asyncio.create_task(self.switch(target))
        self.runner = (target, task)

        def done(t):
            if self.runner and self.runner[1] is t:
                self.runner = None
        task.add_done_callback(done)


hub = Hub()
routes = web.RouteTableDef()


@routes.get("/")
async def index(req):
    return web.FileResponse(HERE / "static" / "index.html", headers={"Cache-Control": "no-cache"})


@routes.get("/api/state")
async def get_state(req):
    return web.json_response(hub.state())


@routes.get("/api/events")
async def events(req):
    ws = web.WebSocketResponse(heartbeat=25)
    await ws.prepare(req)
    hub.clients.add(ws)
    await ws.send_json({"type": "state", **hub.state()})
    try:
        async for _ in ws:
            pass
    finally:
        hub.clients.discard(ws)
    return ws


@routes.post("/api/gateways")
async def add_gateway(req):
    body = await req.json()
    url = body["url"].strip().rstrip("/")
    if not url.startswith("http"):
        url = "http://" + url
    gid = slug(body.get("id") or re.sub(r"^https?://", "", url))
    if gid in hub.gateways:
        raise web.HTTPConflict(text="gateway exists")
    cfg = {"id": gid, "url": url}
    hub.store.data["gateways"].append(cfg)
    hub.store.save()
    hub.add_gateway(cfg)
    return web.json_response(cfg)


@routes.delete("/api/gateways/{id}")
async def del_gateway(req):
    g = hub.gateways.pop(req.match_info["id"], None)
    if g:
        g.task.cancel()
        hub.store.data["gateways"] = [c for c in hub.store.data["gateways"] if c["id"] != g.id]
        hub.store.save()
        await hub.broadcast({"type": "state", **hub.state()})
    return web.json_response({"ok": True})


@routes.post("/api/gateways/{id}/send")
async def gw_send(req):
    g = hub.gateways[req.match_info["id"]]
    await g.post("/api/send", await req.json())
    return web.json_response({"ok": True})


@routes.post("/api/gateways/{id}/commands")
async def gw_save_command(req):
    body = await req.json()
    g = hub.gateways[req.match_info["id"]]
    res = await g.save_command(body["device"], body["name"], body["code"])
    await hub.broadcast({"type": "gateway", "gateway": g.view()})
    return web.json_response(res)


@routes.put("/api/activities")
async def put_activities(req):
    acts = await req.json()
    ids = {a["id"] for a in acts if a.get("id")}
    for a in acts:
        if not a.get("id"):
            base, n = slug(a["name"]), 2
            a["id"] = base
            while a["id"] in ids:
                a["id"], n = f"{base}_{n}", n + 1
            ids.add(a["id"])
    hub.store.data["activities"] = acts
    hub.store.save()
    await hub.broadcast({"type": "state", **hub.state()})
    return web.json_response(acts)


@routes.post("/api/switch")
async def switch(req):
    target = (await req.json()).get("activity")
    if target and target not in {a["id"] for a in hub.store.data["activities"]}:
        raise web.HTTPNotFound(text="no such activity")
    hub.start_switch(target)
    await hub.broadcast({"type": "running", "running": target})
    return web.json_response({"ok": True})


@routes.post("/api/step")
async def test_step(req):
    """Run one step on its own (the editor's play button)."""
    await hub.do_step(await req.json())
    return web.json_response({"ok": True})


@routes.get("/api/ha/entities")
async def ha_entities(req):
    states = await hub.ha("GET", "/states")
    out = [{"entity_id": s["entity_id"], "name": s["attributes"].get("friendly_name", s["entity_id"]), "state": s["state"]}
           for s in states if s["entity_id"].split(".")[0] in HA_DOMAINS]
    return web.json_response(sorted(out, key=lambda e: e["entity_id"]))


@routes.get("/api/ha/services")
async def ha_services(req):
    svcs = await hub.ha("GET", "/services")
    return web.json_response({d["domain"]: sorted(d["services"]) for d in svcs if d["domain"] in HA_DOMAINS | {"homeassistant"}})


@web.middleware
async def errors(req, handler):
    try:
        return await handler(req)
    except web.HTTPException:
        raise
    except KeyError as e:
        return web.json_response({"error": f"unknown {e}"}, status=404)
    except Exception as e:
        log.exception("request %s", req.path)
        return web.json_response({"error": str(e)}, status=500)


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    app = web.Application(middlewares=[errors])
    app.add_routes(routes)
    app.router.add_static("/static", HERE / "static")
    app.on_startup.append(hub.start)
    app.on_cleanup.append(hub.stop)
    log.info("data in %s, Home Assistant %s", DATA, HA_URL or "not configured")
    web.run_app(app, port=PORT, access_log=None)


if __name__ == "__main__":
    main()
