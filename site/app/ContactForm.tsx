"use client";
import { useEffect, useRef, useState } from "react";
import s from "./contact.module.css";

// Messages go to the Vanflame portfolio inbox (/admin → Inbox), tagged with this project.
const ENDPOINT = process.env.NEXT_PUBLIC_CONTACT_ENDPOINT || "https://www.vanflame.dev/api/contact";

type Props = { source: string; title: string; lede: string; interests: string[]; button?: string };

export default function ContactForm({ source, title, lede, interests, button = "Send message" }: Props) {
  const opened = useRef(Date.now());
  const [state, setState] = useState<"idle" | "sending" | "done">("idle");
  const [error, setError] = useState("");
  const vid = useRef("");

  // One anonymous visit per day per site for the portfolio's visitor map (city-level, no IP stored).
  useEffect(() => {
    try {
      let v = localStorage.getItem("vf-vid") || "";
      if (!v) { v = Math.random().toString(36).slice(2, 14) + Date.now().toString(36); localStorage.setItem("vf-vid", v); }
      vid.current = v;
      const day = new Date().toISOString().slice(0, 10), k = "vf-seen-" + source;
      if (localStorage.getItem(k) === day || navigator.webdriver) return;
      localStorage.setItem(k, day);
      navigator.sendBeacon(ENDPOINT.replace(/\/api\/contact$/, "/api/track"),
        new Blob([JSON.stringify({ vid: v, source, page: location.pathname, referrer: document.referrer })], { type: "text/plain" }));
    } catch {}
  }, [source]);

  async function submit(e: React.FormEvent<HTMLFormElement>) {
    e.preventDefault();
    const form = e.currentTarget;
    if (!form.reportValidity()) return;
    const data = new FormData(form);
    const fields: Record<string, string> = {};
    for (const k of ["name", "email", "company", "interest", "message"]) fields[k] = String(data.get(k) || "");
    setState("sending"); setError("");
    try {
      const r = await fetch(ENDPOINT, {
        method: "POST", headers: { "content-type": "application/json" },
        body: JSON.stringify({ source, page: location.href, fields, _hp: data.get("_hp") || "", _t: opened.current, vid: vid.current, tz: Intl.DateTimeFormat().resolvedOptions().timeZone, screen: `${screen.width}x${screen.height}` }),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(d.error || "Something went wrong. Please try again.");
      setState("done");
    } catch (err) {
      setError(err instanceof Error ? err.message : "Something went wrong."); setState("idle");
    }
  }

  return (
    <section id="contact" className={s.wrap}>
      <div className={s.inner}>
        <div className={s.copy}>
          <p className={s.kicker}>Contact</p>
          <h2 className={s.title}>{title}</h2>
          <p className={s.lede}>{lede}</p>
          <ul className={s.points}><li>Reply within 24 hours</li><li>No spam, ever</li><li>Free first consultation</li></ul>
        </div>
        {state === "done" ? (
          <div className={`${s.card} ${s.done}`}>
            <span className={s.check} aria-hidden>✓</span>
            <h3>Message sent!</h3>
            <p>Thanks for reaching out. You&apos;ll get a reply by email soon.</p>
          </div>
        ) : (
          <form className={s.card} onSubmit={submit} noValidate>
            <div className={s.grid}>
              <label className={s.f}><span>Name *</span><input name="name" required autoComplete="name" placeholder="Your name" /></label>
              <label className={s.f}><span>Email *</span><input name="email" type="email" required autoComplete="email" placeholder="Where should we reply?" /></label>
              <label className={s.f}><span>Company</span><input name="company" placeholder="Optional" /></label>
              <label className={s.f}><span>I&apos;m interested in *</span>
                <select name="interest" required defaultValue=""><option value="" disabled>Choose…</option>{interests.map((i) => <option key={i}>{i}</option>)}</select>
              </label>
              <label className={`${s.f} ${s.wide}`}><span>Message *</span><textarea name="message" required rows={5} placeholder="Tell us a bit about what you need" /></label>
            </div>
            <input className={s.hp} name="_hp" tabIndex={-1} autoComplete="off" aria-hidden="true" />
            {error && <p className={s.err} role="alert">{error}</p>}
            <button className={s.btn} type="submit" disabled={state === "sending"}>{state === "sending" ? "Sending…" : button}</button>
            <p className={s.note}>Information submitted through this form is used only to respond to your inquiry and is never sold or shared.</p>
          </form>
        )}
      </div>
    </section>
  );
}
