import { fileURLToPath } from "node:url";
import path from "node:path";

/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  poweredByHeader: false,
  // this site lives in a subfolder of a repo that has its own lockfile
  turbopack: { root: path.dirname(fileURLToPath(import.meta.url)) },
};
export default nextConfig;
