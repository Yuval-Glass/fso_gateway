import Image from "next/image";

/**
 * BER Killerz brand mark.
 *
 * Renders /public/brand-logo.png as-is. We pass `unoptimized` because
 * Next.js's default image pipeline downsamples the RGBA source into an
 * 8-bit palette PNG, which drops per-pixel alpha and can leave a
 * faint rectangular halo around the artwork on dark backgrounds.
 *
 * The source PNG already carries the intended palette, so optimization
 * gives us no benefit and hurts transparency.
 */
// Tight-cropped variant of the logo: the original PNG had ~18% empty space
// on top and ~27% on bottom around the artwork, which made the bear look
// small. /brand-logo-tight.png is the same artwork with margins removed.
const TIGHT_W = 885;
const TIGHT_H = 840;

export function BrandMark({ size = 88 }: { size?: number }) {
  return (
    <Image
      src="/brand-logo-tight.png"
      alt="BER Killerz"
      width={TIGHT_W}
      height={TIGHT_H}
      priority
      unoptimized
      className="select-none"
      style={{
        width: size,
        height: "auto",
        objectFit: "contain",
        filter: "drop-shadow(0 0 14px rgba(0, 212, 255, 0.35))",
      }}
    />
  );
}
