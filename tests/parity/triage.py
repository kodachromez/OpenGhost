"""Reviewed mismatch categories, not numerical tolerances or ignored pixels.

The shared native disconnected notice, absent folder pill/browser toggle and
Qt/Chromium text/gradient rasterization remain in EVERY raw full-window diff.
Primary labels identify the fixture-specific issue, not every changed pixel.
"""
BUG = 'genuine native parity bug'
NOISE = 'font/platform rasterization noise'
INTENTIONAL = 'intentional difference'
NONDETERMINISM = 'test nondeterminism'
MANUAL = 'fixture requiring visible/manual validation'

def classify(f):
    name = f['id']
    if f.get('manual'):
        return MANUAL, f['reason']
    if name.startswith('effort-'):
        return MANUAL, 'Measured headlessly, but this GLX backend rejects RGBA16F layers. Glass/oil pixels are not certified; manual-visible-effort-hdr is excluded from the default run.'
    if f.get('classification'):
        return f['classification'], f.get('notes','')
    if name.startswith(('welcome-', 'appearance-system-', 'composer-', 'models-available')) or name=='sidebar-hidden':
        return INTENTIONAL, 'Shared disconnected-shell differences: visible native refusal, absent reference folder pill/browser toggle. Text and gradient edges also differ.'
    if name in ['thinking','tool-running']:
        return BUG, 'The same activity ghost is vertically offset relative to the preceding user message; not just rasterization.'
    if name in ['tool-completed','chat-basic','chat-streaming','chat-streaming-code'] or name.startswith('chat-') and name.split('-',1)[1] in ['minimum','compact','wide']:
        return NOISE, 'Retained basic text/ghost layout is close after the bubble-width fix; glyph/curve rasterization remains, in addition to the intentional shell differences. Not a pixel-exact result.'
    if name.startswith('diagram-') and name not in ['diagram-editor','diagram-invalid']:
        return NOISE, 'Same parsed diagram family/source; palette and outer margins corrected. Remaining drawing/text antialiasing and shared shell differences are measured, not waived. A representative scene is not exhaustive grammar coverage.'
    if name=='diagram-editor':
        return INTENTIONAL, 'Reference structured visual editor versus native source-text editor; a known unported feature, not implemented by this audit.'
    if name.startswith('auth-') or name.startswith('settings-providers-'):
        return BUG, 'Native provider/auth page omits the reference lead/section/key-field layout. Do not silently equate the prompt-based native login view with the reference inline field.'
    if name.startswith('usage-') or name.startswith('settings-usage-'):
        return BUG, 'Provider order/names/colour cycle corrected; usage page labels, spacing, empty-state/date/chart presentation still differ.'
    if name.startswith('settings-general-') or name.startswith('settings-') and name.split('-',1)[1] in ['minimum','compact','wide']:
        return INTENTIONAL, 'General copy reflects native unavailable pinned-file preparation; text wrapping and small-window layout also differ. No unsupported limits/features advertised just to match text.'
    if name.startswith('settings-appearance-'):
        return BUG, 'Appearance preview/card typography, contours and description layout differ (including platform rasterization).'
    if name.startswith('sidebar-'):
        return BUG, 'Sidebar group/row spacing, folder/draft presentation and interaction affordances differ; source data and clock age are shared.'
    if name.startswith('models-picker') or name=='mode-menu':
        return BUG, 'Stage/veil/glass and label placement differ; software-driver effect limitations may contribute. Not dismissed as font noise.'
    if name.startswith('approval-'):
        return BUG, 'Approval card spacing/column sizing and typography differ with identical presentation object; no answer is sent.'
    if name.startswith('attachments-'):
        return INTENTIONAL, 'Native unavailable sent-file preview, vertical preview rows and missing per-attachment notes differ from the reference cards; no attachment service is added.'
    if name.startswith('scroll-') or name=='selection':
        return BUG, 'Long-message geometry, lazy positioning and selection/veil/jump presentation diverge; repeat deltas separately flag unstable capture.'
    if name in ['markdown-image','markdown-image-stack','markdown-video-card','markdown-image-held'] or name.startswith('media-'):
        return NOISE, 'Same reply pictures/videos from the same synthetic bytes (or the same refused request). CSS backdrop-filter frosting on the play mark, dots and arrows is approximated with translucent fills; edge/glyph rasterization and shared shell differences remain.'
    if name=='markdown-arithmetic':
        return BUG, 'Reference renders a column-arithmetic worksheet; native presents a code block.'
    if name in ['markdown-tex-display','markdown-tex-matrix','markdown-unicode','markdown-quotes','chat-user-quotes','chat-error','chat-auth-error','chat-stopped']:
        return BUG, 'Visible formatting/position/direction or notice/action differences beyond glyph edge noise; see the paired images.'
    if name.startswith('markdown-') or name=='diagram-invalid':
        return NOISE, 'Retained renderer broadly agrees for this small source; text/line/shadow antialiasing remains alongside shared intentional shell differences. No error threshold has been applied.'
    return BUG, f.get('notes','Unresolved presentation difference; not accepted as a match.')
