"""FlowSQL Python Worker — FastAPI 应用（Gateway 架构版）"""

import asyncio
import os
import threading
import time

from fastapi import FastAPI, Request, Response, HTTPException
from fastapi.responses import JSONResponse
import httpx
import uvicorn

from .arrow_codec import decode_arrow_ipc, encode_arrow_ipc, decode_arrow_ipc_file, encode_arrow_ipc_file
from .config import WorkerConfig
from .operator_registry import OperatorRegistry

app = FastAPI(title="FlowSQL Python Worker")
registry = OperatorRegistry()


def _do_reload():
    """重载算子的共享逻辑"""
    config = WorkerConfig.from_args()
    old_keys = set(f"{op['catelog']}.{op['name']}" for op in registry.list_operators())
    registry.reload(config.operators_dir)
    new_keys = set(f"{op['catelog']}.{op['name']}" for op in registry.list_operators())
    added = new_keys - old_keys
    removed = old_keys - new_keys
    print(f"Worker _do_reload: added={len(added)}, removed={len(removed)}")
    return added, removed


@app.get("/operators/python/health")
async def health():
    return {"status": "ok"}


@app.get("/operators/python/list")
async def list_operators():
    return registry.list_operators()


@app.post("/operators/python/reload")
async def reload_operators():
    """重新扫描算子目录"""
    loop = asyncio.get_event_loop()
    added, removed = await loop.run_in_executor(None, _do_reload)
    return {"added": list(added), "removed": list(removed)}


@app.post("/operators/python/work/{catelog}/{name}")
async def work(catelog: str, name: str, request: Request):
    op = registry.get(catelog, name)
    if not op:
        raise HTTPException(status_code=404, detail=f"Operator {catelog}.{name} not found")

    body = await request.json()
    input_path = body.get("input")
    if not input_path:
        raise HTTPException(status_code=400, detail="Missing 'input' field in request body")

    try:
        df_in = decode_arrow_ipc_file(input_path)
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Failed to decode Arrow IPC file: {e}")

    try:
        df_out = op.work(df_in)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Operator error: {e}")

    output_path = input_path.replace("_in", "_out")

    try:
        encode_arrow_ipc_file(df_out, output_path)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Failed to encode Arrow IPC file: {e}")

    return JSONResponse(content={"output": output_path})


@app.post("/operators/python/configure/{catelog}/{name}")
async def configure(catelog: str, name: str, request: Request):
    body = await request.json()
    key = body.get("key", "")
    value = body.get("value", "")

    if not registry.configure(catelog, name, key, value):
        raise HTTPException(status_code=404, detail=f"Operator {catelog}.{name} not found")

    return {"status": "ok"}


def _register_with_gateway(gateway_addr: str, service_name: str, local_addr: str, prefixes: list):
    """向 Gateway 注册路由"""
    for prefix in prefixes:
        try:
            resp = httpx.post(
                f"http://{gateway_addr}/gateway/register",
                json={"prefix": prefix, "address": local_addr, "service": service_name},
                timeout=3,
            )
            if resp.status_code == 200:
                print(f"Worker: registered route {prefix} -> {local_addr}")
            else:
                print(f"Worker: failed to register route {prefix}: {resp.text}")
        except Exception as e:
            print(f"Worker: failed to register route {prefix}: {e}")


def _heartbeat_loop(gateway_addr: str, service_name: str, interval: int):
    """心跳线程"""
    while True:
        try:
            httpx.post(
                f"http://{gateway_addr}/gateway/heartbeat",
                json={"service": service_name},
                timeout=2,
            )
        except Exception:
            pass
        time.sleep(interval)


def main():
    config = WorkerConfig.from_args()

    # 1. 发现并注册算子
    registry.discover(config.operators_dir)
    operators = registry.list_operators()

    print(f"FlowSQL Worker starting on {config.host}:{config.port}")
    print(f"Operators directory: {config.operators_dir}")
    print(f"Registered operators: {len(operators)}")

    # 2. 向 Gateway 注册路由（如果有 Gateway 地址）
    gateway_addr = os.environ.get("FLOWSQL_GATEWAY_ADDR")
    if gateway_addr:
        local_addr = f"{config.host}:{config.port}"
        prefixes = ["/operators/python/health", "/operators/python/list",
                    "/operators/python/reload", "/operators/python/work",
                    "/operators/python/configure"]
        _register_with_gateway(gateway_addr, "pyworker", local_addr, prefixes)

        # 启动心跳线程
        t = threading.Thread(target=_heartbeat_loop, args=(gateway_addr, "pyworker", 5), daemon=True)
        t.start()

    # 3. 启动 HTTP 服务
    try:
        uvicorn.run(app, host=config.host, port=config.port, log_level="warning")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
