"""FlowSQL Control Protocol 客户端"""

import json
import socket
import threading
import time
from typing import Callable, Dict, Any, Optional


class ControlClient:
    """FlowSQL Control Protocol 客户端 - 用于 Python Worker 与 C++ 后端通信"""

    PROTOCOL_VERSION = "1.0"

    def __init__(self, socket_path: str):
        self.socket_path = socket_path
        self.sock: Optional[socket.socket] = None
        self.running = False
        self.message_thread: Optional[threading.Thread] = None
        self.handlers: Dict[str, Callable] = {}
        self.send_lock = threading.Lock()

    def connect(self, timeout: int = 5) -> bool:
        """连接到 C++ 控制服务器"""
        try:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect(self.socket_path)
            print(f"ControlClient: connected to {self.socket_path}")
            return True
        except Exception as e:
            print(f"ControlClient: connect failed: {e}")
            if self.sock:
                self.sock.close()
                self.sock = None
            return False

    def disconnect(self):
        """断开连接"""
        self.running = False
        if self.message_thread:
            self.message_thread.join(timeout=2)
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
        print("ControlClient: disconnected")

    def send_message(self, msg_type: str, payload: Dict[str, Any]) -> bool:
        """发送消息"""
        if not self.sock:
            print("ControlClient: not connected")
            return False

        message = {
            "version": self.PROTOCOL_VERSION,
            "type": msg_type,
            "timestamp": int(time.time()),
            "payload": payload
        }

        try:
            msg_json = json.dumps(message) + "\n"
            with self.send_lock:
                self.sock.sendall(msg_json.encode('utf-8'))
            return True
        except Exception as e:
            print(f"ControlClient: send_message failed: {e}")
            return False

    def send_worker_ready(self, operators: list) -> bool:
        """发送 Worker 就绪通知"""
        return self.send_message("worker_ready", {"operators": operators})

    def send_operator_added(self, category: str, name: str,
                           description: str = "", position: str = "DATA") -> bool:
        """发送算子添加通知"""
        return self.send_message("operator_added", {
            "category": category,
            "name": name,
            "description": description,
            "position": position
        })

    def send_operator_removed(self, category: str, name: str) -> bool:
        """发送算子移除通知"""
        return self.send_message("operator_removed", {
            "category": category,
            "name": name
        })

    def send_heartbeat(self, stats: Dict[str, Any]) -> bool:
        """发送心跳"""
        return self.send_message("heartbeat", {"stats": stats})

    def send_error(self, code: int, message: str, details: str = "") -> bool:
        """发送错误报告"""
        return self.send_message("error", {
            "code": code,
            "message": message,
            "details": details
        })

    def register_handler(self, msg_type: str, handler: Callable):
        """注册消息处理器"""
        self.handlers[msg_type] = handler

    def start_message_loop(self):
        """启动消息接收循环（异步）"""
        self.running = True
        self.message_thread = threading.Thread(target=self._message_loop, daemon=True)
        self.message_thread.start()

    def _message_loop(self):
        """消息接收循环"""
        buffer = ""
        while self.running:
            try:
                if not self.sock:
                    break

                data = self.sock.recv(4096)
                if not data:
                    print("ControlClient: connection closed by server")
                    break

                buffer += data.decode('utf-8')
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    if line.strip():
                        self._dispatch_message(line)

            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    print(f"ControlClient: message_loop error: {e}")
                break

    def _dispatch_message(self, msg_json: str):
        """分发消息到处理器"""
        try:
            msg = json.loads(msg_json)
            msg_type = msg.get("type")
            payload = msg.get("payload", {})

            handler = self.handlers.get(msg_type)
            if handler:
                handler(payload)
            else:
                print(f"ControlClient: no handler for message type: {msg_type}")

        except Exception as e:
            print(f"ControlClient: dispatch_message error: {e}")
