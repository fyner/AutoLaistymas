// ========================================
// BENDROS FUNKCIJOS VISIEMS PUSLAPIAMS
// ========================================

// ========================================
// TEMA (Theme Management)
// ========================================

function getCurrentTheme() {
  return localStorage.getItem('theme') || 'light';
}

function setTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  localStorage.setItem('theme', theme);
  updateThemeUI(theme);
}

function updateThemeUI(theme) {
  const icon = document.getElementById('themeIcon');
  if (icon) {
    if (theme === 'dark') {
      icon.textContent = '☀️';
    } else {
      icon.textContent = '🌙';
    }
  }
}

function toggleTheme() {
  const currentTheme = getCurrentTheme();
  const newTheme = currentTheme === 'light' ? 'dark' : 'light';
  setTheme(newTheme);
}

// ========================================
// PRANEŠIMAI (Messages)
// ========================================

function showMessage(text, type = 'info'){
  const bar = document.getElementById('globalMsg');
  if (!bar) return;
  
  // Jei pranešimas tuščias, paslepti juostą
  if (!text || text === '') {
    bar.classList.remove('show', 'ok', 'err', 'warning');
    bar.classList.add('hidden');
    return;
  }
  
  // Parodyti pranešimą su spalva iš karto
  bar.classList.remove('hidden');
  bar.textContent = text;
  bar.className = 'global show ' + type; // Pridedame 'show' klasę iš karto
  
  // VISI pranešimai dingsta automatiškai po 3 sekundžių
  if (text && type !== 'info') {
    setTimeout(() => {
      if (bar.textContent === text) { // Patikriname, ar pranešimas nebuvo pakeistas
        bar.classList.add('fade-out');
        setTimeout(() => {
          bar.textContent = '';
          bar.className = 'global hidden';
        }, 200); // Palaukiame, kol animacija baigsis (optimizuota)
      }
    }, 3000);
  }
}

// ========================================
// AUTENTIFIKACIJA (Authentication)
// ========================================

function buildAuthHeaders(){ 
  return {}; 
}

// ========================================
// PAGALBINĖS FUNKCIJOS (Utility Functions)
// ========================================

function pad(n){ 
  return String(n).padStart(2,'0'); 
}

function toIsoNoTZ(d){
  return d.getFullYear()+"-"+pad(d.getMonth()+1)+"-"+pad(d.getDate())+"T"+pad(d.getHours())+":"+pad(d.getMinutes())+":"+pad(d.getSeconds());
}

function isValidHHMM(s){ 
  return /^\d{2}:\d{2}$/.test(s) && 
         Number(s.slice(0,2))>=0 && Number(s.slice(0,2))<=23 && 
         Number(s.slice(3,5))>=0 && Number(s.slice(3,5))<=59; 
}

// ========================================
// UI REFRESH INTERVAL
// ========================================

function getRefreshMs(){
  const v = parseInt(localStorage.getItem('uiRefreshMs')||'1000', 10);
  return (Number.isFinite(v) && v >= 250 ? v : 1000);
}

function setRefreshMs(value){
  const v = parseInt(value, 10);
  if(!Number.isFinite(v) || v < 250){ 
    showMessage('UI intervalas: Neteisinga reikšmė. Minimali: 250 ms', 'err'); 
    return false; 
  }
  localStorage.setItem('uiRefreshMs', String(v));
  showMessage('UI intervalas: Išsaugotas sėkmingai', 'ok');
  return true;
}

// ========================================
// INICIALIZACIJA (Initialization)
// ========================================

// Inicializuoti temą kai puslapis užkraunamas
document.addEventListener('DOMContentLoaded', function() {
  const savedTheme = getCurrentTheme();
  setTheme(savedTheme);
});
