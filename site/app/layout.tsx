import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "SIM800 Gateway — 16-SIM ESP32 SMS gateway firmware",
  description: "ESP32 firmware that drives 16 SIM800L modems through one multiplexer, forwards every SMS to your backend, and updates itself over the air.",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <head>
        <link rel="preconnect" href="https://fonts.googleapis.com" />
        <link rel="preconnect" href="https://fonts.gstatic.com" crossOrigin="" />
        <link href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@500;600;700&family=IBM+Plex+Sans:wght@400;500;600&family=JetBrains+Mono:wght@400;600&display=swap" rel="stylesheet" />
      </head>
      <body>{children}</body>
    </html>
  );
}
