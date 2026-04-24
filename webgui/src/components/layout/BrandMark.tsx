import Image from "next/image";

/**
 * BER Killerz brand mark.
 * Uses the real logo PNG at /public/brand-logo.png, recolored via CSS filter
 * to match the dashboard's cyan/blue palette (red → cyan, brown → muted teal).
 *
 * To update the source image, replace /public/brand-logo.png — no code changes needed.
 */
export function BrandMark({
  size = 88,
  variant = "tinted",
}: {
  size?: number;
  /** "tinted" keeps logo detail with cyan shift. "mono" is a flat cyan silhouette. */
  variant?: "tinted" | "mono";
}) {
  const filter =
    variant === "mono"
      ? "brightness(0) saturate(100%) invert(58%) sepia(97%) saturate(400%) hue-rotate(158deg) brightness(101%) contrast(101%) drop-shadow(0 0 14px rgba(0, 212, 255, 0.55))"
      : "hue-rotate(175deg) saturate(1.35) brightness(1.08) contrast(1.08) drop-shadow(0 0 14px rgba(0, 212, 255, 0.45))";

  return (
    <Image
      src="/brand-logo.png"
      alt="BER Killerz"
      width={size}
      height={size}
      priority
      className="select-none"
      style={{
        filter,
        width: size,
        height: "auto",
        objectFit: "contain",
        mixBlendMode: "screen",
      }}
    />
  );
}
