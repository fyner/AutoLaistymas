(function(){
  const $s = sel => document.querySelector(sel);
  const $t = (sel, v) => ($s(sel).textContent = v);

  async function getStatus(){
    try{
      const r = await fetch('/status');
      if(!r.ok) throw new Error('HTTP '+r.status);
      const j = await r.json();
      $t('#s-temp', (j.temp===-999?'-':j.temp.toFixed? j.temp.toFixed(1)+' °C' : j.temp));
      $t('#s-hum', (j.hum===-999?'-':j.hum.toFixed? j.hum.toFixed(1)+' %' : j.hum));
      $t('#s-pres', (j.pres===-999?'-':j.pres.toFixed? j.pres.toFixed(1)+' hPa' : j.pres));
      $t('#s-water', j.waterLevel || '-');
      $t('#s-state', j.state || '-');
      $t('#s-remaining', (j.remainingTimeSec!=null? j.remainingTimeSec+' s' : '-'));
      $t('#s-time', j.currentTime || '-');
      // RTC validumo laukas nėra API dalis, todėl inferuojam: jei yra currentTime ir ne 0000, laikom true
      $t('#s-rtcValid', (j.currentTime && !/^0000/.test(j.currentTime))? 'true':'false');
    }catch(e){
      console.error(e);
      $t('#status-msg', 'Nepavyko gauti būsenos: '+e.message);
    }
  }

  async function getConfig(){
    try{
      const r = await fetch('/config');
      if(!r.ok) throw new Error('HTTP '+r.status);
      const j = await r.json();
      $s('#config-json').textContent = JSON.stringify(j, null, 2);
    }catch(e){
      console.error(e);
      $s('#config-json').textContent = 'Nepavyko gauti konfigūracijos: '+e.message;
    }
  }

  async function startWatering(){
    try{
      const r = await fetch('/start');
      const text = await r.text();
      if(!r.ok) throw new Error(text || ('HTTP '+r.status));
      $t('#status-msg','Laistymas pradėtas');
      await getStatus();
    }catch(e){
      $t('#status-msg','Nepavyko pradėti laistymo: '+e.message);
    }
  }

  async function stopWatering(){
    try{
      const r = await fetch('/stop');
      const text = await r.text();
      if(!r.ok) throw new Error(text || ('HTTP '+r.status));
      $t('#status-msg','Laistymas sustabdytas');
      await getStatus();
    }catch(e){
      $t('#status-msg','Nepavyko sustabdyti: '+e.message);
    }
  }

  async function setTime(){
    const val = $s('#in-time').value.trim();
    try{
      const r = await fetch('/config/time',{
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body: JSON.stringify({time: val})
      });
      const text = await r.text();
      if(!r.ok) throw new Error(text || ('HTTP '+r.status));
      $t('#time-msg','RTC laikas atnaujintas: '+val);
      await getStatus();
    }catch(e){
      $t('#time-msg','Klaida nustatant laiką: '+e.message);
    }
  }

  // Events
  $s('#btn-refresh').addEventListener('click', getStatus);
  $s('#btn-start').addEventListener('click', startWatering);
  $s('#btn-stop').addEventListener('click', stopWatering);
  $s('#btn-set-time').addEventListener('click', setTime);

  // Init
  getStatus();
  getConfig();
  setInterval(getStatus, 2000);
})();
