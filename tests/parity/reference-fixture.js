// Applied to the unmodified reference page, via DevTools, never saved under reference/.
window.parityFixture = async f => {
  const style = document.createElement('style');
  // Pin generic UI fonts on both sides; leave serif/math fallback stacks intact.
  style.textContent = ':root { --font: "Noto Sans"; --mono-font: "Noto Sans Mono"; } textarea,input { caret-color: transparent !important; }';
  document.head.append(style);
  await Theme.set(f.theme || 'dark');
  await document.fonts.ready;
  const list = document.querySelector('.thread-list');
  list.replaceChildren();
  for (const row of f.rows || []) {
    const entry = row.role === 'user'
      ? {role:'user',text:row.text,attachments:row.attachments||[]}
      : {role:'assistant',content:row.text};
    let el;
    if(row.role === 'error') {
      const view=chat.assistantMessage();
      view.status.remove();
      chat.fail({reconciled:false},view,row.error);
      el=view.el;
    } else if(row.state === 'live') {
      const view=chat.assistantMessage();
      view.status.remove();
      el=view.el;
      list.append(el);
      view.stream.push(row.text);
    } else if(row.role === 'note') {
      el = document.createElement('div'); el.className='message is-assistant';
      chat.note({el}, row.text);
    } else el = chat.entryView(entry, row.copyable ?? row.role === 'assistant');
    if(el) list.append(el);
    if(row.joined && el) el.style.marginTop='-14px';
  }
  main.classList.toggle('is-empty', !(f.rows?.length));
  composerInput.value=f.draft||'';
  composerText.refresh(); syncComposer();
  if(f.models) {
    settings.models=f.models.map(m=>({...m,id:`${m.provider}:${m.id}`,api:m.id,efforts:['off','low','medium','high'],defaultEffort:'medium',vision:true}));
    settings.model=settings.models[0].id; settings.preferredEffort='medium'; settings.changed(); settings.show(settings.model); modelStage.sync();
  }
  if(f.sessions) {
    library.chats=f.sessions.map(s=>({...s,updated:Date.now(),created:Date.now(),...(s.folder?{}:{space:s.id}),folder:s.folder||''}));
    library.folders=[...new Set(f.sessions.map(s=>s.folder).filter(Boolean))].map(path=>({path,collapsed:false}));
    chatList.render();
  }
  if(f.cards) {
    attachments.items=f.cards.map(c=>({...c,info:FileKinds.describe(c.name),payload:{type:'text',text:'fixture'},note:''}));
    for(const c of attachments.items) attachments.row.append(attachments.chip(c,false));
    syncComposer();
  }
  if(f.approval) {
    const card=new ApprovalCard(f.approval);
    list.append(card.el);
    card.open(!!f.details,false);
  }
  if(f.providers) {
    settings.providers=f.providers.map(p=>({...p,methods:[
      {type:'apiKey',label:'API key',hint:'Use an API key or sign in.',url:'',placeholder:'API key'},
      {type:'oauth',label:'Account',hint:'Use an API key or sign in.',action:'Sign in'}
    ]}));
    settings.providerIndex=new Map(settings.providers.map(p=>[p.id,p]));
    settings.status=Object.fromEntries(f.providers.map(p=>[p.id,{connected:p.connected,keySaved:p.connected,waiting:f.authState==='waiting',error:p.note,account:{}}]));
    settings.build();
  }
  if(f.usage || f.usageCounts) {
    await Usage.ready;
    for(const count of f.usageCounts || [{provider:'fixture',model:'example',modelName:'Example',input:1200,output:340,cached:400,requests:2}]) Usage.record(count);
    await new Promise(resolve=>setTimeout(resolve,0));
  }
  if(f.diagram && !Diagram.compile(f.diagram,['lilac','turquoise','blue','pink','yellow','orange','green'],{width:976,column:680,zoom:1})) throw Error('Invalid reference diagram fixture: '+f.id);
  return true;
};
