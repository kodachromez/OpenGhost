"""Shared, synthetic presentation data; no credentials, provider or backend calls."""
import json

def fixtures():
    result = []
    def add(id, **kw):
        result.append(dict(id=id, **kw))
    def reply(text, **kw):
        return dict(role='assistant', key=kw.pop('key', 'answer'), text=text, **kw)
    prompt = dict(role='user', key='user', text='Show me the result.')
    models = [dict(provider='openai',id='gpt-5',name='GPT-5'),
              dict(provider='anthropic',id='claude-sonnet',name='Claude Sonnet'),
              dict(provider='deepseek',id='deepseek-chat',name='DeepSeek Chat')]
    for theme in ['dark','light']:
        add('welcome-'+theme,theme=theme)
        for page in ['general','providers','usage','appearance']:
            add('settings-'+page+'-'+theme,theme=theme,
                native=f"settingsDialog.open(); settingsDialog.show('{page}',true);",
                reference=f"settings.open(); settings.page('{page}',true);")
    for label,w,h in [('minimum',760,540),('compact',900,640),('wide',1600,1000)]:
        add('welcome-'+label,width=w,height=h)
        add('chat-'+label,width=w,height=h,rows=[prompt,reply('A short **answer**, at a representative window size.')])
        add('settings-'+label,width=w,height=h,native="settingsDialog.open(); settingsDialog.show('general',true);",reference="settings.open(); settings.page('general',true);")
    for name,theme,system in [('system-dark','system',True),('system-light','system',False)]:
        add('appearance-'+name,theme=theme,systemDark=system)
    add('welcome-hidpi',width=900,height=640,dpr=2)
    add('markdown-hidpi',width=900,height=640,dpr=2,rows=[reply('## Crisp text\n\n**Bold**, *italic* and `code` at 2×.\n\n\\\\[x^2 + y^2 = 1\\\\]')])
    for name,wait in [('flight',800),('word',2600),('handoff',3500),('finished',5000)]:
        add('splash-'+name,motion=True,splash=True,wait=wait,classification='test nondeterminism',notes='Real shader/motion path sampled after load; startup frame gating and clocks are not phase locked. Repeated captures measure uncertainty, not a certified temporal match.')
    add('welcome-motion',motion=True,wait=900,classification='test nondeterminism',notes='Idle ghost sampled with normal motion; not a phase-locked comparison.')
    add('sidebar-hidden',native='window.sidebarOpen=false;',reference="setSidebarCollapsed(true);")
    sessions=[dict(id='s1',title='Pinned research',pinned=True),dict(id='s2',title='A recent conversation'),dict(id='s3',title='A deliberately long conversation title that must truncate'),dict(id='s4',title='Project notes',folder='/fixture/project')]
    add('sidebar-populated',sessions=sessions)
    add('sidebar-search',sessions=sessions,keepFocus=True,native="sidebar.focusSearch();",reference="searchField.focus(); searchField.hover(true);")
    add('sidebar-empty-search',keepFocus=True,native="sidebar.focusSearch(); sidebar.sessions.query='missing';",properties=[dict(object='search',name='text',value='missing')],reference="searchField.focus(); searchInput.value='missing'; searchField.update(); chatList.setQuery('missing');")
    add('sidebar-delete-confirm',sessions=sessions,native="sidebar.askDelete('s2');",reference="chatList.askDelete('s2');")
    add('sidebar-rename',sessions=sessions,keepFocus=True,reference="chatList.rename('s2');",nativeAfter="sidebar.startRename('s2',1);")
    add('composer-draft',draft='Explain this carefully, please.')
    add('composer-multiline',draft='First line\nSecond line\nThird line\nFourth line')
    add('composer-long',draft='\n'.join('Line %d: keep the composer bounded.'%i for i in range(1,24)))
    add('composer-quote',draft='> A quoted passage\n\nExplain this.')
    add('models-available',models=models)
    add('models-picker',models=models,native='modelStage.openStage(false);',reference='modelStage.open(false);')
    add('models-picker-light',theme='light',models=models,native='modelStage.openStage(false);',reference='modelStage.open(false);')
    for level in ['off','low','medium','high']:
        add('effort-'+level,models=models,native=f"window.settings.chooseThinking('{level}'); thinkingChoice.toggle(false);",reference=f"settings.effort='{level}'; effortSlider.setEfforts(settings.efforts); effortSlider.open();",nativeAfter='',notes='Settled effort stage, not a timing/spring equivalence claim.')
    add('mode-menu',native='modeDock.show(false);',reference="modeButton.click();")
    add('chat-basic',rows=[prompt,reply('Hello. How can I help?')])
    add('chat-user-quotes',rows=[dict(prompt,text='> Quoted source\n\nMy question about it.'),reply('Here is the explanation.')])
    add('chat-stopped',rows=[prompt,reply('An incomplete answer.'),dict(role='note',key='stop',text='Stopped')])
    for name,text,code,action in [('error','The request failed. Try again.','network_error','retry'),('auth-error','Sign in to continue.','auth_required','open_settings')]:
        add('chat-'+name,rows=[prompt,dict(role='error',key='failure',text=text,error=dict(message=text,code=code,action=action,retryable=True))],propertiesAfter=[dict(object='retryTurn',name='visible',value=True)],notes='Native display Note projection with its existing Retry affordance enabled; no retry dispatched. Reference uses chat.fail on the same error.')
    add('chat-streaming',rows=[prompt,reply('The answer is **still arriving**.\n\nHere is the next paragraph',state='live',copyable=False)],notes='Same frozen partial source in both live parsers; reduced motion settles text without ending the response.')
    add('chat-streaming-code',rows=[prompt,reply('```python\\nfor i in range(3):\\n    print(i)',state='live',copyable=False)])
    for name in ['thinking','tool-running']:
        add(name,rows=[prompt],native='window.working=true;',reference="{const v=chat.assistantMessage(); document.querySelector('.thread-list').append(v.el); chat.showGhost(v);}",notes='Reference shows a working ghost, not reasoning or tool-result panels.')
    add('tool-completed',rows=[prompt,reply('The operation finished.')],notes='No tool result panel is present in the reference chat path.')
    add('timing',rows=[prompt,reply('A measured answer.',metrics='150 tokens · %1',preview='Input: 100 · Output: 50')],classification='intentional difference',notes='Native per-reply timing has no equivalent in reference chat.js; reference entry carries usage but does not draw an inline timer.')
    long=[prompt]+[reply(f'## Section {i}\n\nA paragraph with **bold** and a [link](https://example.com).\n\n'+('A long conversation stays readable. '*7),key=f'a{i}') for i in range(1,12)]
    add('scroll-top',rows=long,wait=900)
    add('scroll-middle',rows=long,wait=900,afterWait=700,scroll=1000,nativeAfter='transcript.seek(1000);')
    add('scroll-bottom',rows=long,wait=900,afterWait=700,scroll=99999,nativeAfter='transcript.resume();')
    # Markdown grammar and unsupported-media behavior, fed to real message renderers.
    markdown={
        'headings':'# Heading one\n\n## Heading two\n\n### Heading three\n\n#### Heading four\n\nA **bold**, *italic*, ~~deleted~~ and `inline code` sentence.',
        'lists':'- First item\n- Second item\n  - Nested item\n\n1. Number one\n2. Number two\n\n- [x] Completed\n- [ ] Pending',
        'quotes':'> A clear quotation.\n>\n> Another paragraph.\n> — Ada Lovelace\n\n> [!WARNING]\n> A useful caution.',
        'table':'| Name | Amount | State |\n| :--- | ---: | :---: |\n| Alpha | 12 | **ready** |\n| Beta | 345 | pending |',
        'code-cpp':'```cpp\n#include <iostream>\n// A deterministic example\nint main() {\n  const int answer = 42;\n  std::cout << "Hello" << answer;\n}\n```',
        'code-python':'```python\n# A comment\ndef greet(name: str) -> str:\n    return f"Hello {name}"\n```',
        'code-js':'```javascript\nconst values = [1, 2, 3];\nconsole.log(values.map(x => x * 2));\n```',
        'code-json':'```json\n{"ready": true, "count": 42, "items": ["one", "two"]}\n```',
        'code-diff':'```diff\n-old line\n+new line\n unchanged\n```',
        'code-long':'```text\n'+('an_unbroken_identifier_'*10)+'\n```',
        'arithmetic':'```\n  123\n+ 456\n─────\n  579\n```',
        'tex-inline':r'An inline expression \(x^2 + \frac{a}{b}\), followed by prose.',
        'tex-display':'\\[\n\\int_0^1 x^2\\,dx = \\frac{1}{3}\n\\]',
        'tex-matrix':'\\[\n\\begin{pmatrix}a & b \\\\ c & d\\end{pmatrix}\n\\]',
        'links':'Visit [example](https://example.com) or <https://example.com>.\n\n---\n\nAfter the rule.',
        'unicode':'English · Ελληνικά · Кириллица\n\n日本語 中文 한글\n\nالعربية · עברית\n\nEmoji: 👻 ✅ 🌍',
        'html-literal':'<script>alert("not executed")</script>\n\n**Untrusted** content stays text.',
        'image':'![Fixture image](https://parity.invalid/image.png)',
        'image-stack':'![First](https://parity.invalid/image.png)\n![Second](https://parity.invalid/image.png)',
        'video-card':'[Example · Author · 4:40](https://www.youtube.com/watch?v=abcdefghijk)',
    }
    # A tiny, synthetic checkerboard, generated identically for both adapters.
    import base64, io
    from PIL import Image, ImageDraw
    im=Image.new('RGB',(240,120),'#386594'); draw=ImageDraw.Draw(im); draw.rectangle((20,20,120,100),fill='#7ecba9'); draw.ellipse((140,30,220,110),fill='#e9b94f')
    buf=io.BytesIO(); im.save(buf,format='PNG'); png=base64.b64encode(buf.getvalue()).decode()
    for name,text in markdown.items():
        extra = dict(imageData=png,reference="{const asks=document.querySelectorAll('.md-gallery-ask'); if(!asks.length) throw Error('Missing image consent controls'); asks.forEach(el=>el.click());}",nativeClick='mediaAsk',notes='Both adapters click the image consent control; the request is fulfilled from synthetic local bytes (CDP in the reference, an injected MediaLoader fetch natively). No network.') if name in ['image','image-stack'] else {}
        add('markdown-'+name,rows=[reply(text)],**extra)
    add('markdown-light',theme='light',rows=[reply(markdown['headings']+'\n\n'+markdown['code-cpp'])])
    for name in ['single','multiple']:
        cards=[dict(name='notes.txt',size=1200,mime='text/plain')]
        if name=='multiple': cards += [dict(name='report.pdf',size=2400000,mime='application/pdf'),dict(name='main.cpp',size=3456,mime='text/plain')]
        add('attachments-sent-'+name,rows=[dict(prompt,attachments=cards)])
        add('attachments-composer-'+name,cards=cards)
    add('markdown-image-held',rows=[reply(markdown['image'])],notes='Both show the click-to-load plate for a picture from an untrusted place; no request is made.')
    # Reply pictures and videos (media-embed.js): trusted places load by themselves, from
    # synthetic bytes on both sides; anything not served stays a link or a plain card.
    trusted='https://upload.wikimedia.org/wikipedia/commons/parity.png'
    thumb=lambda id: f'https://i.ytimg.com/vi/{id}/hq720.jpg'
    media={
        'gallery-trusted':(f'![A trusted picture]({trusted})',[trusted]),
        'gallery-source':(f'[![A captioned picture]({trusted})](https://example.com/page)',[trusted]),
        'gallery-lost':(f'![Not there]({trusted})',[]),
        'gallery-mixed':(f'Pictures:\n![Trusted]({trusted})\n![Elsewhere](https://parity.invalid/image.png)',[trusted]),
        'video-thumb':('[A talk · A channel · 4:40](https://www.youtube.com/watch?v=abcdefghijk)',[thumb('abcdefghijk')]),
        'videos-many':('[First talk · One](https://youtu.be/aaaaaaaaaaa) [Second talk · Two](https://youtu.be/bbbbbbbbbbb) [Third talk · Three · 12:05](https://youtu.be/ccccccccccc)',[thumb(x*11) for x in 'abc']),
    }
    for name,(text,served) in media.items():
        add('media-'+name,rows=[reply(text)],imageData=png,mediaUrls=served,
            notes='Served addresses fulfilled from synthetic bytes in both adapters; any other request fails. No network.')
    add('media-streaming',rows=[reply(f'![First]({trusted})\n![Second](https://upload.wiki',state='live',copyable=False)],
        notes='Pictures still being written wait as one plate in both renderers; nothing is requested.')
    add('attachment-image',rows=[dict(prompt,attachments=[dict(name='sample.png',mime='image/png',size=len(buf.getvalue()),image=True,url='data:image/png;base64,'+png,width=240,height=120)])],classification='intentional difference',notes='Reference image bytes are display data; native sent-card projection only retains metadata, so no thumbnail is fabricated.')
    for kind,effect in [('command','run'),('file','change'),('web','online')]:
        info=dict(kind=kind,effect=effect,badge=True,title='Review this operation',places=[dict(kind='file' if kind=='file' else 'folder',label='project',title='/fixture/project')],reveal='changes' if kind=='file' else 'command')
        if kind=='file': info.update(removed='old value\nsecond line',added='new value\nsecond line')
        else: info['code']='printf "%s\\n" "hello"' if kind=='command' else 'https://example.com'
        for opened in [False,True]:
            add('approval-'+kind+('-expanded' if opened else ''),rows=[prompt],approval=info,details=opened)
    providers=[dict(id='fixture',name='Fixture provider',hint='Use an API key or sign in.',connected=False,logout=False,oauth=True,apiKey=True,note='',error=False)]
    for state in ['disconnected','connected','waiting','error','key']:
        p=dict(providers[0]); p['connected']=state=='connected'; p['logout']=p['connected']; p['note']='Invalid API key' if state=='error' else ''; p['error']=state=='error'
        login=dict(id='login',providerId='fixture',type='prompt',promptId='key',input=True,secret=True,placeholder='API key',message='Enter your API key.') if state=='key' else dict(id='login',providerId='fixture',type='info',message='Waiting for sign-in…') if state=='waiting' else {}
        add('auth-'+state,providers=[p],login=login,authState=state,native="settingsDialog.open(); settingsDialog.show('providers',true);",reference="settings.open(); settings.page('providers',true);",notes='Same provider state; native prompt-based auth layout diverges from reference inline key field.')
    for name in ['empty','populated']:
        add('usage-'+name,usage=name=='populated',native="settingsDialog.open(); settingsDialog.show('usage',true);", reference="settings.open(); settings.page('usage',true);")
    add('usage-multiple',usageCounts=[dict(provider='deepseek',model='one',input=1200,output=340,cached=400,requests=2),dict(provider='openai',model='two',input=800,output=100,cached=100,requests=1)],providers=[dict(providers[0],id='deepseek',name='First provider',oauth=False,apiKey=False),dict(providers[0],id='openai',name='Second provider',oauth=False,apiKey=False)],native="settingsDialog.open(); settingsDialog.show('usage',true);", reference="settings.open(); settings.page('usage',true);")
    # All 43 dispatch families in reference diagram.js::KINDS, not only Mermaid's subset.
    diagrams={
        'flow':'flowchart LR\nA[Start] --> B{Ready?}\nB -->|yes| C[Done]\nB -->|no| A',
        'state':'stateDiagram-v2\n[*] --> Idle\nIdle --> Active: begin\nActive --> [*]',
        'sequence':'sequenceDiagram\nparticipant A as Client\nparticipant B as Server\nA->>B: Request\nB-->>A: Response',
        'pie':'pie title Budget\n"Work" : 60\n"Rest" : 40',
        'xy':'xychart-beta\ntitle Progress\nx-axis [Mon, Tue, Wed]\ny-axis "Count" 0 --> 10\nline [2, 5, 8]',
        'candles':'candlestick\ntitle Price\n2026-09-01, 100, 110, 95, 105, 1000\n2026-09-02, 105, 115, 101, 108, 1200',
        'timeline':'timeline\ntitle Roadmap\n2024 : Plan\n2025 : Build\n2026 : Ship',
        'gantt':'gantt\ntitle Project\ndateFormat YYYY-MM-DD\nsection Work\nDesign :a, 2026-09-01, 5d\nBuild :b, after a, 8d',
        'mindmap':'mindmap\n  root((Project))\n    Design\n      Layout\n    Build\n      Test',
        'quadrant':'quadrantChart\ntitle Priorities\nx-axis Low --> High\ny-axis Low --> High\nA: [0.2, 0.4]\nB: [0.7, 0.8]',
        'radar':'radar-beta\ntitle Options\naxis s[Speed], q[Quality], c[Cost]\ncurve a[Alpha]{80, 70, 60}\ncurve b[Beta]{60, 80, 75}',
        'er':'erDiagram\nUSER ||--o{ ORDER : places\nUSER {\n int id\n string name\n}\nORDER {\n int id\n}',
        'class':'classDiagram\nAnimal <|-- Bird\nclass Animal {\n+name string\n+move()\n}\nclass Bird {\n+fly()\n}',
        'wireframe':'wireframe\ntitle Example\nnav Site\n  links Home, About\nhero Welcome\n  text Build something useful\n  button Start\nfeatures Benefits\n  Fast: Ready today\n  Clear: Easy to use',
        'files':'files\ntitle Project\npath /fixture/project\nnotes.txt | 1.2 KB | 2026-09-01\nsrc/ | 2 items\n  main.cpp | 3.4 KB\nmore 3',
        'metrics':'metrics\ntitle Today\nWork | 42 tasks | +4 good | 20, 28, 35, 42\nTime | 8 h | of 10',
        'bars':'bars\ntitle Budget\nunit USD\nDesign | 120 | First\nBuild | 240 | Next\ntotal',
        'ranges':'ranges\ntitle Readings\nPulse | 72 bpm | 60-100\nPressure | 34 psi | 30-36',
        'plan':'plan\nMon\n  Design | layout\nTue\n  Build | components',
        'steps':'steps\n[x] Prepare | 5 min | tools\n  Clear a space\nAssemble | 10 min | screwdriver\n  ! Check the screws',
        'journey':'journey\ntitle Work day\nsection Morning\nPlan: 5: User\nBuild: 3: User',
        'waterfall':'waterfall\nStart | 124\nCosts | -52\nIncome | +30\nResult | total',
        'funnel':'funnel\nVisitors | 1000\nReaders | 400\nSignups | 80',
        'sankey':'sankey-beta\nIncome,Needs,60\nIncome,Savings,40\nNeeds,Home,35\nNeeds,Food,25',
        'heatmap':'heatmap\ncols A, B, C\nOne | 2 | 5 | 8\nTwo | 9 | 3 | 6',
        'scatter':'scatter\nx-axis Cost\ny-axis Value\nAlpha | 2 | 7\nBeta | 5 | 8\nGamma | 8 | 3',
        'treemap':'treemap-beta\n"Work"\n  "Design": 30\n  "Build": 50\n"Rest": 20',
        'git':'gitGraph\ncommit\nbranch feature\ncheckout feature\ncommit\ncheckout main\nmerge feature',
        'array':'array\nStart | 1, 3, 5, 7 | lo: 0, hi: 3 | 1..2\nNext | 1, 3, 5, 7 | mid: 1 | 1',
        'bracket':'bracket\nRound 1\n  Alpha 2 - 1 Beta\n  Gamma 3 - 0 Delta\nFinal\n  Alpha 1 - 2 Gamma',
        'nutrition':'nutrition\nCalories | 1850 kcal | of 2200\nProtein | 132 g | of 150\nFat | 60 g\nCarbs | 190 g\nBreakfast: oatmeal | 420 kcal | P 18 · F 12 · C 58',
        'facts':'facts\nfile report.pdf | 42 pages | 1.8 MB\nStatus | Ready | Reviewed | good\nVersion | 1.3 | Current',
        'checklist':'checklist\n[x] Ready | Passed\n[!] Review | Check this\n[-] Missing | Not available\n[?] Unknown\n[ ] Pending',
        'changes':'changes\nPrice | 20 -> 18 | Discount | good\nVersion | 1.2 -> 1.3 | Updated',
        'outline':'outline\n1. Introduction | p. 3 | Purpose\n  1.1 Scope | p. 4 | Boundaries\n2. Results | p. 8 | Findings',
        'matches':'matches\nFinals\n18:00 | Alpha - Beta | Cup | Live | *\nGamma 2 : 1 Delta',
        'words':'words\nBasics\nhello | [həˈləʊ] | greeting | Hello there — a greeting\nworld | [wɜːld] | Earth',
        'gloss':'gloss\nI | *like* | tea\nfirst person | enjoy | drink\nsubject | verb | object\n= I like tea.',
        'forms':'forms\ncols Present, Past\nyo | hablo | hablé\ntú | hablas | hablaste',
        'recipe':'recipe\ntitle Porridge\nabout 10 min | 2 servings | 320 kcal\nIngredients\n  Oats | 100 g\n  Milk | 250 ml\nSteps\n  Simmer | 5 min | Stir\n  ! Do not burn',
        'parts':'parts\nCPU | Example 8-core | Fits | 240 | good\nRAM | 32 GB | Matched | 100\ntotal',
        'settings':'settings\nPower\nSettings > Battery > Saving | on | Longer life\nSettings > Display > Brightness | 60% | Comfortable',
        'route':'route\nDay 1\nStation | 09:00 | Begin\n  train | 2 h | 200 km\nCity | 11:00 | Museum',
    }
    assert len(diagrams)==43
    for name,source in diagrams.items():
        add('diagram-'+name,diagram=source,rows=[reply('```mermaid\n'+source+'\n```')],wait=700)
    add('diagram-editor',rows=[reply('```mermaid\n'+diagrams['flow']+'\n```')],wait=800,invoke=dict(object='diagramBlock',method='open'),referenceAfter="document.querySelector('.dg-tools [data-action=edit]').click();",notes='Reference structured editor versus native source editor; no new editor features added.')
    add('selection',afterWait=700,rows=[reply('Select this sentence for a follow-up question.')],nativeAfter="Selection.selectAll('answer',transcript.itemAtIndex(0).rich);",referenceAfter="{const p=document.querySelector('.message-content p'); const r=document.createRange(); r.selectNodeContents(p); const s=getSelection(); s.removeAllRanges(); s.addRange(r); document.dispatchEvent(new Event('selectionchange')); document.dispatchEvent(new PointerEvent('pointerup',{bubbles:true}));}")
    add('diagram-invalid',rows=[reply('```mermaid\nnot-a-diagram\n???\n```')])
    add('diagram-light',theme='light',diagram=diagrams['flow'],rows=[reply('```mermaid\n'+diagrams['flow']+'\n```')])
    # Reference-only views: capture their real absence on native, do not implement features.
    add('add-menu',reference="document.querySelector('.composer-add').dispatchEvent(new CustomEvent('add'));",classification='intentional difference',notes='Native + opens a file chooser directly; reference dock is unported. No native dialog launched.')
    add('attachment-note',cards=[dict(name='notes.txt',size=1200,mime='text/plain')],referenceAfter="attachments.openNote(attachments.items[0]);",classification='intentional difference',notes='Attachment note popover is unported.')
    add('drop-overlay',reference="main.classList.add('is-dropping');",native="window.notice=window.fileRefusal;",classification='intentional difference',notes='Native refuses drop/paste files explicitly instead of displaying reference drop art.')
    add('stats-card',reference="document.querySelector('.thread-list').append(StatsCard.build({version:1,models:[{id:'fixture:model',name:'Fixture model',input:100,output:50,cached:20,written:0,requests:1}],turns:[{m:0,t:150,c:20}],mini:null,context:{used:150,window:1000},uncounted:0})); main.classList.remove('is-empty');",classification='intentional difference',notes='Reference stats card has no native transcript delegate.')
    add('compact-notice',reference="document.querySelector('.thread-list').append(chat.compactNotice(false)); main.classList.remove('is-empty');",classification='intentional difference',notes='Native keeps but does not draw compact rows.')
    add('mini-chat',reference="chat.active.record={id:'fixture',title:'Fixture',folder:'',space:'fixture'}; MiniChat.open({settings,source:chat,library});",classification='intentional difference',notes='Reference mini-chat dialog is unported; frontend state alone does not supply a view.')
    add('lock-screen',reference="lockScreen.show({id:'fixture',locked:true},false);",classification='intentional difference',notes='Native lock UI and production sealing are intentionally unavailable.')
    add('lock-password',reference="lockScreen.show({id:'fixture',locked:true},false); lockScreen.ask();",classification='intentional difference',notes='Reference password field only. No actual password, crypto or backend operation.')
    for id,reason in [
        ('manual-visible-effort-hdr','Offscreen GLX logs unsupported RGBA16F texture layers. Headless effort diffs remain recorded, but the glass/oil effect needs separate GPU validation; no visible fallback is run.'),
        ('manual-visible-file-dialog','Native system file chooser depends on portal/desktop theme; not opened by headless suite.'),
        ('manual-visible-folder-picker','Reference host folder picker and native absent entry point; desktop/portal operation excluded.'),
        ('manual-visible-window-chrome','Compositor decorations, snapping, activation, minimize/maximize and desktop DPI migration require a real window.'),
        ('manual-visible-ime-accessibility','IME candidate windows, screen reader and physical focus/input routing are OS integration, not offscreen pixels.'),
        ('manual-visible-browser-host','Reference browser panel requires Electron webview host; native browser panel is intentionally absent. No remote site is loaded.'),
    ]: add(id,manual=True,reason=reason)
    return result

if __name__ == '__main__':
    print(json.dumps(dict(fixtures=fixtures()),indent=2))
