"""Raw RGB byte measurements. No registration, blur, masking or pass tolerance."""
import numpy as np

def measure(a, b):
    if a.shape != b.shape or a.ndim != 3 or a.shape[2] != 3 or not a.size:
        raise ValueError('Expected equal, nonempty RGB images')
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    per = d.max(axis=2)
    return dict(changed_pixels=int(np.count_nonzero(per)), total_pixels=int(per.size),
                changed_percent=round(float(np.mean(per > 0) * 100), 5),
                over8_percent=round(float(np.mean(per > 8) * 100), 5),
                mae=round(float(d.mean()), 6),
                rmse=round(float(np.sqrt(np.mean(d.astype(float) ** 2))), 6),
                max_channel=int(d.max()))
