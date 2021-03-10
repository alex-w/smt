// Stellarium Web - Copyright (c) 2020 - Stellarium Labs SAS
//
// This program is licensed under the terms of the GNU AGPL v3, or
// alternatively under a commercial licence.
//
// The terms of the AGPL v3 license can be found in the main directory of this
// repository.
//
// This file is part of the Survey Monitoring Tool plugin, which received
// funding from the Centre national d'études spatiales (CNES).

import SmtLayerPage from './components/smt-layer-page.vue'
import storeModule from './store'
import Vue from 'vue'
import VueGoogleCharts from 'vue-google-charts'
import qe from './query-engine'
import filtrex from 'filtrex'
import sprintfjs from 'sprintf-js'

Vue.use(VueGoogleCharts)

export default {
  vuePlugin: {
    install: (Vue, options) => {
      Vue.component('smt-layer-page', SmtLayerPage)
    }
  },
  name: 'SMT',
  storeModule: storeModule,
  panelRoutes: [
    { path: '/p/smt', component: SmtLayerPage, meta: { tabName: 'Survey Tool', prio: 1 } }
  ],
  onEngineReady: function (app) {
    app.$store.commit('setValue', { varName: 'SMT.status', newValue: 'initializing' })

    // Init base view settings
    app.$stel.core.lines.equatorial.visible = true
    app.$stel.core.lines.boundary.visible = true
    app.$stel.core.atmosphere.visible = false
    app.$stel.core.landscapes.visible = false
    app.$stel.core.landscapes.fog_visible = false
    app.$stel.core.cardinals.visible = false
    app.$stel.core.planets.visible = false
    app.$stel.core.dsos.visible = false
    app.$stel.core.comets.visible = false
    app.$stel.core.satellites.visible = false
    app.$stel.core.minor_planets.visible = false
    app.$stel.core.mount_frame = app.$stel.FRAME_ICRF
    app.$stel.core.projection = 5 // PROJ_MOLLWEIDE
    app.$stel.core.observer.refraction = false
    app.$stel.core.observer.yaw = 0
    app.$stel.core.observer.pitch = 0
    app.$stel.core.fov = 270
    app.$stel.core.time_speed = 0
    app.$store.commit('setValue', { varName: 'showLocationButton', newValue: false })
    app.$store.commit('setValue', { varName: 'showTimeButtons', newValue: false })
    app.$store.commit('setValue', { varName: 'showFPS', newValue: true })
    app.$store.commit('setValue', { varName: 'showNightmodeButton', newValue: false })
    app.$store.commit('setValue', { varName: 'showAzimuthalGridButton', newValue: false })
    app.$store.commit('setValue', { varName: 'showLandscapeButton', newValue: false })
    app.$store.commit('setValue', { varName: 'showAtmosphereButton', newValue: false })
    app.$store.commit('setValue', { varName: 'showConstellationsArtButton', newValue: false })
    app.$store.commit('setValue', { varName: 'showPlanetsVisibilityMenuItem', newValue: false })
    app.$store.commit('setValue', { varName: 'showViewSettingsMenuItem', newValue: false })
    app.$store.commit('setValue', { varName: 'showObservingPanelTabsButtons', newValue: false })

    // Add all data sources.
    const doUrl = 'https://stellarium.sfo2.cdn.digitaloceanspaces.com/'
    const core = app.$stel.core
    core.stars.addDataSource({ url: doUrl + 'swe-data-packs/minimal-2020-01-31-4c9cbfbe/stars', key: 'minimal' })
    core.stars.addDataSource({ url: doUrl + 'swe-data-packs/base-2020-01-31-8b663edc/stars', key: 'base' })
    core.stars.addDataSource({ url: doUrl + 'swe-data-packs/extended-2020-01-31-176f8caf/stars', key: 'extended' })
    core.stars.addDataSource({ url: doUrl + 'surveys/gaia/v1', key: 'gaia' })
    core.milkyway.addDataSource({ url: doUrl + 'surveys/milkyway/v1' })
    core.dss.addDataSource({ url: doUrl + 'surveys/dss/v1' })
    core.dsos.addDataSource({ url: doUrl + 'swe-data-packs/base-2020-01-31-8b663edc/dso' })
    core.dsos.addDataSource({ url: doUrl + 'swe-data-packs/extended-2020-01-31-176f8caf/dso' })
    app.dataSourceInitDone = true

    return qe.init().then(smtConfig => {
      const filtrexOptions = {
        extraFunctions: { sprintf: (fmt, x) => sprintfjs.sprintf(fmt, x) }
      }
      for (const field of smtConfig.fields) {
        if (field.formatFunc) {
          field.formatFuncCompiled = filtrex.compileExpression(field.formatFunc, filtrexOptions)
        }
      }

      Vue.prototype.$smt = smtConfig

      app.$store.commit('setValue', { varName: 'SMT.smtServerInfo', newValue: qe.smtServerInfo })
      if (smtConfig.watermarkImage) {
        app.$store.commit('setValue', { varName: 'SMT.watermarkImage', newValue: smtConfig.watermarkImage })
      }
      app.$store.commit('setValue', { varName: 'SMT.status', newValue: 'ready' })
    },
    err => {
      app.$store.commit('setValue', { varName: 'SMT.status', newValue: 'error' })
      throw err
    })
  }
}
