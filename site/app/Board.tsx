"use client";
import { useEffect, useState } from "react";

const CARRIERS = ["GLOBE", "SMART", "DITO", "TNT"];
type Msg = { slot: number; text: string; at: string };

export default function Board() {
  const [active, setActive] = useState<number | null>(null);
  const [log, setLog] = useState<Msg[]>([]);
  const [signal] = useState(() => Array.from({ length: 16 }, (_, i) => 2 + ((i * 7) % 3)));

  useEffect(() => {
    const id = setInterval(() => {
      const slot = Math.floor(Math.random() * 16);
      const code = String(Math.floor(100000 + Math.random() * 900000));
      setActive(slot);
      setLog((l) => [{ slot, text: `Your verification code is ${code}`, at: new Date().toLocaleTimeString([], { hour12: false }) }, ...l].slice(0, 4));
      setTimeout(() => setActive(null), 900);
    }, 1800);
    return () => clearInterval(id);
  }, []);

  return (
    <div className="board" aria-label="Live simulation of the 16-SIM gateway">
      <div className="board__head mono">
        <span><i className="dot dot--ok" /> ESP32 · v1.0.21</span>
        <span>MUX CD74HC4067</span>
      </div>
      <div className="slots">
        {Array.from({ length: 16 }, (_, i) => (
          <div key={i} className={`slot ${active === i ? "is-rx" : ""}`}>
            <span className="slot__led" />
            <span className="slot__id mono">SIM{String(i + 1).padStart(2, "0")}</span>
            <span className="slot__car mono">{CARRIERS[i % 4]}</span>
            <span className="slot__sig" aria-hidden>{Array.from({ length: 4 }, (_, b) => <i key={b} className={b < signal[i] ? "on" : ""} />)}</span>
          </div>
        ))}
      </div>
      <div className="feed mono">
        <p className="feed__title">→ FORWARDED TO BACKEND</p>
        {log.length === 0 && <p className="feed__row muted">waiting for SMS…</p>}
        {log.map((m, i) => (
          <p key={m.at + i} className="feed__row"><span className="amber">[{m.at}]</span> SIM{String(m.slot + 1).padStart(2, "0")} · {m.text}</p>
        ))}
      </div>
    </div>
  );
}
