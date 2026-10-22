import { createRouter, createWebHistory } from 'vue-router'

const routes = [
  { path: '/', redirect: '/dashboard' },
  { path: '/explore', name: 'Explore', component: { render: () => null } },
  { path: '/dashboard', name: 'Dashboard', component: () => import('../views/Dashboard.vue') },
  { path: '/channels', name: 'Channels', component: () => import('../views/Channels.vue') },
  { path: '/operators', name: 'Operators', component: () => import('../views/Operators.vue') },
  { path: '/tasks', name: 'Tasks', component: () => import('../views/Tasks.vue') },
  { path: '/tasks/runtime/:taskId', name: 'TaskRuntime', component: () => import('../views/TaskRuntime.vue') }
]

export default createRouter({
  history: createWebHistory(),
  routes
})
