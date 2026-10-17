
"""Penyimpanan riwayat sesi dan paket sensor menggunakan SQLite."""

import json
import sqlite3
from pathlib import Path
from typing import Any


def inisialisasi_db(lokasi: str | Path = "data/health_monitor.db") -> Path:
    """Membuat database dan tabel jika belum tersedia."""
    path = Path(lokasi)
    path.parent.mkdir(parents=True, exist_ok=True)

    with sqlite3.connect(path) as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS sesi (
                nama_sesi TEXT PRIMARY KEY,
                waktu TEXT NOT NULL,
                catatan_json TEXT NOT NULL
            )
        """)
        conn.execute("""
            CREATE TABLE IF NOT EXISTS paket_sensor (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                nama_sesi TEXT NOT NULL,
                node_id TEXT NOT NULL,
                seq INTEGER NOT NULL,
                t_kirim REAL NOT NULL,
                t_terima REAL NOT NULL,
                latency_ms REAL NOT NULL,
                jitter_ms REAL,
                bpm REAL,
                spo2 REAL,
                suhu REAL,
                hilang INTEGER NOT NULL DEFAULT 0,
                FOREIGN KEY (nama_sesi) REFERENCES sesi(nama_sesi),
                UNIQUE (nama_sesi, node_id, seq)
            )
        """)

    return path


def simpan_sesi(
    nama_sesi: str,
    ringkasan: dict[str, Any],
    paket_per_node: dict[str, list[dict[str, Any]]],
    lokasi: str | Path = "data/health_monitor.db",
) -> Path:
    """Simpan ringkasan sesi dan paket sensor ke SQLite."""
    path = inisialisasi_db(lokasi)

    with sqlite3.connect(path) as conn:
        conn.execute("PRAGMA foreign_keys = ON")
        conn.execute("""
            INSERT INTO sesi (nama_sesi, waktu, catatan_json)
            VALUES (?, ?, ?)
            ON CONFLICT(nama_sesi) DO UPDATE SET
                waktu = excluded.waktu,
                catatan_json = excluded.catatan_json
        """, (
            nama_sesi,
            ringkasan["waktu"],
            json.dumps(ringkasan.get("catatan", {}), ensure_ascii=False),
        ))

        for node_id, baris in paket_per_node.items():
            for p in baris:
                conn.execute("""
                    INSERT INTO paket_sensor (
                        nama_sesi, node_id, seq, t_kirim, t_terima,
                        latency_ms, jitter_ms, bpm, spo2, suhu, hilang
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ON CONFLICT(nama_sesi, node_id, seq) DO UPDATE SET
                        t_kirim = excluded.t_kirim,
                        t_terima = excluded.t_terima,
                        latency_ms = excluded.latency_ms,
                        jitter_ms = excluded.jitter_ms,
                        bpm = excluded.bpm,
                        spo2 = excluded.spo2,
                        suhu = excluded.suhu,
                        hilang = excluded.hilang
                """, (
                    nama_sesi,
                    node_id,
                    int(p["seq"]),
                    float(p["t_kirim"]),
                    float(p["t_terima"]),
                    float(p["latency_ms"]),
                    float(p["jitter_ms"]) if p["jitter_ms"] not in (None, "") else None,
                    p.get("bpm") or None,
                    p.get("spo2") or None,
                    p.get("suhu") or None,
                    int(p.get("hilang_sejak_paket_terakhir", 0)),
                ))

    return path