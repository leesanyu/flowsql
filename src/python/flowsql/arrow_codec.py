"""Arrow IPC 编解码"""

import pyarrow as pa
import polars as pl


def decode_arrow_ipc(data: bytes) -> pl.DataFrame:
    """将 Arrow IPC stream 字节解码为 Polars DataFrame"""
    reader = pa.ipc.open_stream(data)
    try:
        table = reader.read_all()
        return pl.from_arrow(table)
    finally:
        reader.close()


def encode_arrow_ipc(df: pl.DataFrame) -> bytes:
    """将 Polars DataFrame 编码为 Arrow IPC stream 字节"""
    table = df.to_arrow()
    batches = table.to_batches()
    if not batches:
        batches = [pa.record_batch([], schema=table.schema)]

    sink = pa.BufferOutputStream()
    writer = pa.ipc.new_stream(sink, batches[0].schema)
    try:
        for batch in batches:
            writer.write_batch(batch)
    finally:
        writer.close()
    return sink.getvalue().to_pybytes()


def decode_arrow_ipc_file(path: str) -> pl.DataFrame:
    """从文件 memory_map 读取 Arrow IPC → Polars DataFrame（零拷贝）"""
    mmap = pa.memory_map(path, 'r')
    try:
        reader = pa.ipc.open_stream(mmap)
        try:
            table = reader.read_all()
            return pl.from_arrow(table)
        finally:
            reader.close()
    finally:
        mmap.close()


def encode_arrow_ipc_file(df: pl.DataFrame, path: str):
    """Polars DataFrame → Arrow IPC 写入文件（零拷贝）"""
    table = df.to_arrow()
    batches = table.to_batches()
    if not batches:
        batches = [pa.record_batch([], schema=table.schema)]

    with open(path, 'wb') as f:
        writer = pa.ipc.new_stream(f, batches[0].schema)
        try:
            for batch in batches:
                writer.write_batch(batch)
        finally:
            writer.close()
