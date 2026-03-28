-- FlowSQL Web 管理系统数据库 Schema

-- 通道注册表
CREATE TABLE IF NOT EXISTS channels (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    category TEXT NOT NULL,
    name TEXT NOT NULL,
    type TEXT NOT NULL DEFAULT 'dataframe',
    schema_json TEXT DEFAULT '[]',
    status TEXT NOT NULL DEFAULT 'active',
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(category, name)
);
