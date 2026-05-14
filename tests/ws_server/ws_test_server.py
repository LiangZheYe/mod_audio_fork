import asyncio
import websockets
from datetime import datetime

async def handle(ws, path=None):
    print(f"[CONNECT] {datetime.now().isoformat()}")
    try:
        async for message in ws:
            pass  # 接收上行音频
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        print(f"[DISCONNECT] {datetime.now().isoformat()}")

async def main():
    async with websockets.serve(handle, "0.0.0.0", 8080):
        print("WebSocket test server listening on port 8080")
        await asyncio.Future()  # run forever

asyncio.run(main())
