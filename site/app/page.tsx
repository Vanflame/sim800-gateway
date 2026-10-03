import ContactForm from "./ContactForm";
import Board from "./Board";

const REPO = "https://github.com/Vanflame/sim800-gateway";

const FEATURES = [
  ["16-SIM polling", "Shared TX, multiplexed RX: one ESP32 UART serves sixteen SIM800L modems in turn."],
  ["Backend forwarding", "Every incoming SMS is parsed and POSTed to your API with SIM, sender and timestamp."],
  ["Local web UI", "Configure Wi-Fi, monitor SIMs, read logs and run tests straight from the device."],
  ["Dual Wi-Fi", "Setup access point and backend station mode running at the same time."],
  ["HTTPS OTA", "Pull new firmware from GitHub Releases or any URL; no cable, no downtime."],
  ["Missed-call OTPs", "Turns missed-call verifications into OTP messages your backend understands."],
  ["USSD balance", "Bulk *143# balance checks across every SIM with one tap."],
  ["SIM watchdog", "Detects unresponsive modems and recovers them automatically."],
  ["Persistent logs", "Message and error logs survive reboots on LittleFS."],
];

const SPECS = [
  ["Controller", "ESP32 Dev Module"],
  ["Modems", "16 × SIM800L GSM"],
  ["Multiplexer", "CD74HC4067, 16-channel"],
  ["Power", "5 V · 10 A+ (2 A peak per modem)"],
  ["Storage", "LittleFS (message + error logs)"],
  ["Updates", "HTTPS OTA (GitHub Releases / custom URL)"],
  ["Sync", "Heartbeat + full inventory every 30 min"],
];

const PINS = [
  ["GPIO5", "TX → all SIM800L RX (shared)"],
  ["GPIO4", "RX ← mux COM"],
  ["S0–S3", "Mux channel select"],
  ["EN", "Mux enable (active low)"],
];

export default function Page() {
  return (
    <main>
      <nav className="nav wrap">
        <a className="brand" href="#"><span className="chip" />SIM800<b>GATEWAY</b></a>
        <div className="nav__links mono"><a href="#features">Features</a><a href="#specs">Specs</a><a href="#flow">Flow</a><a href="#contact">Contact</a><a className="btn btn--sm" href={REPO}>GitHub ↗</a></div>
      </nav>

      <header className="hero wrap">
        <div>
          <p className="tag mono"><i className="dot dot--ok" /> Open-source ESP32 firmware</p>
          <h1>Sixteen SIMs.<br /><span>One tiny board.</span></h1>
          <p className="lede">An SMS gateway you can hold in your hand. It polls up to 16 SIM800L modems through a single multiplexer, forwards every message to your backend, and updates itself over the air.</p>
          <div className="cta"><a className="btn" href={REPO}>View firmware</a><a className="btn btn--ghost" href="#specs">Hardware specs</a></div>
          <dl className="kpis mono">
            <div><dt>SIMS</dt><dd>16</dd></div><div><dt>UARTS</dt><dd>1</dd></div><div><dt>OTA</dt><dd>HTTPS</dd></div>
          </dl>
        </div>
        <Board />
      </header>

      <section id="features" className="wrap sec">
        <p className="eyebrow mono">// 01 · CAPABILITIES</p>
        <h2>Built like infrastructure,<br />sized like a hobby board.</h2>
        <div className="grid">
          {FEATURES.map(([t, d], i) => (
            <article key={t} className="card"><span className="mono num">{String(i + 1).padStart(2, "0")}</span><h3>{t}</h3><p>{d}</p></article>
          ))}
        </div>
      </section>

      <section id="flow" className="wrap sec">
        <p className="eyebrow mono">// 02 · SIGNAL PATH</p>
        <h2>From the tower to your API.</h2>
        <div className="schematic">
          {["GSM network", "SIM800L × 16", "CD74HC4067 mux", "ESP32 parser", "Your backend"].map((s, i, a) => (
            <div key={s} className="node"><span className="mono node__i">{String(i + 1).padStart(2, "0")}</span><b>{s}</b>{i < a.length - 1 && <span className="wire" aria-hidden />}</div>
          ))}
        </div>
      </section>

      <section id="specs" className="wrap sec specs">
        <div>
          <p className="eyebrow mono">// 03 · HARDWARE</p>
          <h2>Spec sheet.</h2>
          <table className="table mono"><tbody>{SPECS.map(([k, v]) => <tr key={k}><th>{k}</th><td>{v}</td></tr>)}</tbody></table>
        </div>
        <div>
          <p className="eyebrow mono">// PINOUT</p>
          <h2>Wiring.</h2>
          <table className="table mono"><tbody>{PINS.map(([k, v]) => <tr key={k}><th className="amber">{k}</th><td>{v}</td></tr>)}</tbody></table>
          <p className="note">⚠ Power matters: SIM800L modules spike to 2 A each. Use a 5 V / 10 A+ supply with good decoupling.</p>
        </div>
      </section>

      <section className="wrap sec final">
        <h2>Flash it. Plug in SIMs.<br /><span>Start receiving.</span></h2>
        <a className="btn" href={REPO}>Get the firmware ↗</a>
      </section>

      <ContactForm source="SIM800 Gateway" title="Want a gateway like this?" lede="Need the firmware for your own SIM bank, a custom build, or help wiring it to your backend? Send a message." interests={["Use this gateway for my business", "Custom firmware / hardware build", "Backend integration", "Something else"]} />

      <footer className="wrap foot mono"><span>SIM800 GATEWAY · ESP32 FIRMWARE</span><span>Built by Vanflame</span></footer>
    </main>
  );
}
