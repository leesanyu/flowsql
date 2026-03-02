import { createRouter, createWebHistory } from 'vue-router'
import Dashboard from '../views/Dashboard.vue'
import Channels from '../views/Channels.vue'
import Operators from '../views/Operators.vue'
import Tasks from '../views/Tasks.vue'

const routes = [
  { path: '/', redirect: '/dashboard' },
  { path: '/dashboard', name: 'Dashboard', component: Dashboard },
  { path: '/channels', name: 'Channels', component: Channels },
  { path: '/operators', name: 'Operators', component: Operators },
  { path: '/tasks', name: 'Tasks', component: Tasks }
]

export default createRouter({
  history: createWebHistory(),
  routes
})
